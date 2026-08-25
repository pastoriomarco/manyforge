#include "manyforge_scene/canonical_empty_scene_owner.hpp"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/logical_scene_projection.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_persistence/sqlite_read_only_preflight.hpp>

#include "embedded_canonical_empty_scene_schema.hpp"
#include "logical_scene_codec_internal.hpp"
#include "sqlite_scene_lifecycle_internal.hpp"

#if SQLITE_VERSION_NUMBER < 3037000
#error "canonical-empty scene owner requires SQLite 3.37.0 or newer"
#endif

#ifndef SQLITE_OPEN_NOFOLLOW
#error "canonical-empty scene owner requires SQLITE_OPEN_NOFOLLOW"
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace manyforge::scene {
namespace {

using core::ActiveBundlePublicationGeneration;
using core::ActiveBundlePublicationReceiptDTO;
using core::ActiveBundlePublicationRequestDTO;
using core::ActiveProjectionRuntimeRecoveryResultDTO;
using core::ActiveProjectionRuntimeRecoveryState;
using core::ActiveProjectionRuntimeSetDTO;
using core::ApplyGeneration;
using core::ApplyId;
using core::BootstrapGeneration;
using core::BootstrapId;
using core::BootstrapProjectionSetSourceDTO;
using core::BootstrapPublicationTransactionDTO;
using core::CellId;
using core::CollisionPolicySnapshot;
using core::ContentDigest;
using core::ErrorDTO;
using core::GraphRevision;
using core::LogicalSceneBootstrapIdentityInputDTO;
using core::LogicalSceneSnapshotDTO;
using core::LogicalSceneSnapshotRoot;
using core::NamedApplyProjectionSetSourceDTO;
using core::NamedApplyPublicationTransactionDTO;
using core::NativeProjectionRecoveryGeneration;
using core::ProjectionAggregateHealthDTO;
using core::ProjectionApplyActivationDTO;
using core::ProjectionApplyActivationResultDTO;
using core::ProjectionApplyActivationResultState;
using core::ProjectionApplyDiscardDTO;
using core::ProjectionApplyDiscardResultDTO;
using core::ProjectionApplyDiscardResultState;
using core::ProjectionApplyFinalizeDTO;
using core::ProjectionApplyFinalizeResultDTO;
using core::ProjectionApplyFinalizeResultState;
using core::ProjectionApplyStageDTO;
using core::ProjectionApplyStageResultDTO;
using core::ProjectionApplyStageResultState;
using core::ProjectionApplyStatusDTO;
using core::ProjectionApplyStatusQueryDTO;
using core::ProjectionApplyStatusState;
using core::ProjectionBootstrapAcceptedDTO;
using core::ProjectionBootstrapEvidenceDTO;
using core::ProjectionBootstrapFinalizeDTO;
using core::ProjectionBootstrapFinalizeResultDTO;
using core::ProjectionBootstrapFinalizeResultState;
using core::ProjectionBootstrapIntentDTO;
using core::ProjectionBootstrapPrerequisiteQueryDTO;
using core::ProjectionBootstrapPrerequisiteState;
using core::ProjectionBootstrapPrerequisiteStatusDTO;
using core::ProjectionBootstrapRebuildRequestDTO;
using core::ProjectionBootstrapRebuildResultDTO;
using core::ProjectionBootstrapRebuildResultState;
using core::ProjectionBootstrapStatusDTO;
using core::ProjectionBootstrapStatusQueryDTO;
using core::ProjectionBootstrapStatusState;
using core::ProjectionHealthState;
using core::ProjectionRevisionDTO;
using core::ProjectionStageGeneration;
using core::ProjectionTransitionEvidenceDTO;
using core::ProjectionTransitionEvidenceState;
using core::RequestId;
using core::RequestOwnerEpoch;
using core::RetainedBundleDTO;
using core::RetainedBundleId;
using core::SceneCommitResultDTO;
using core::SceneId;
using core::SceneObjectSnapshotDTO;
using core::SceneObjectStateDTO;
using core::SceneProjectionCommitStatusDTO;
using core::SceneResourceId;
using core::SceneRevision;
using core::SceneTransactionRequestDTO;
using core::SceneUpdateStatusDTO;

constexpr int kBusyTimeoutMilliseconds = 10000;
constexpr std::int64_t kPageSizeBytes = 4096;
constexpr std::int64_t kMaximumPageCount =
    kMaximumSceneStoreDatabasePageImageBytes / kPageSizeBytes;
constexpr std::int64_t kExpectedBaseSchemaObjectCardinality = 24;
constexpr std::int64_t kExpectedTransactionSchemaObjectCardinality = 42;
constexpr std::int64_t kExpectedNativeHandoffSchemaObjectCardinality = 66;
constexpr std::int64_t kExpectedSchemaObjectCardinality = 71;
constexpr std::int64_t kLogicalSceneTransactionStoreSchemaVersion = 4;
constexpr std::string_view kLogicalSceneTransactionStoreSchemaId =
    "manyforge.internal.logical-scene.n5b.v4";
constexpr std::int64_t kNativeProjectionHandoffStoreSchemaVersion = 5;
constexpr std::string_view kNativeProjectionHandoffStoreSchemaId =
    "manyforge.internal.logical-scene.n5c.v5";
constexpr std::uint64_t kMaximumSafeIntegral = 9007199254740991ULL;
constexpr int kMaximumSqliteValueBytes = 1024 * 1024;
constexpr std::size_t kMaximumLogicalSceneOwnerEvents =
    kMaximumLogicalSceneTransactions + kMaximumNamedApplySceneStages;

class CellAdmissionTemporarilyUnavailable final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] void ThrowCellAdmissionFailure(
    const std::string_view context, const core::ErrorDTO &error) {
  const auto message = std::string(context) + ": " + error.message;
  if (error.code == "kernel_owner_unavailable") {
    throw CellAdmissionTemporarilyUnavailable(message);
  }
  throw CanonicalEmptySceneOwnerConflictError(message);
}

[[noreturn]] void ThrowSqlite(sqlite3 *database, const int result,
                              const std::string_view context) {
  const std::string detail =
      database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
  if (result == SQLITE_FULL || result == SQLITE_TOOBIG ||
      result == SQLITE_NOMEM) {
    throw CanonicalEmptySceneOwnerLimitError(
        std::string(context) + ": storage limit exhausted: " + detail);
  }
  if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
    throw CanonicalEmptySceneOwnerBusyError(std::string(context) + ": " +
                                            detail);
  }
  if (result == SQLITE_CONSTRAINT) {
    throw CanonicalEmptySceneOwnerConflictError(std::string(context) + ": " +
                                                detail);
  }
  if (result == SQLITE_CORRUPT || result == SQLITE_NOTADB ||
      result == SQLITE_SCHEMA) {
    throw CanonicalEmptySceneOwnerIntegrityError(std::string(context) + ": " +
                                                 detail);
  }
  throw CanonicalEmptySceneOwnerError(std::string(context) + ": " + detail);
}

void Check(sqlite3 *database, const int result,
           const std::string_view context) {
  if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW) {
    ThrowSqlite(database, result, context);
  }
}

void Exec(sqlite3 *database, const std::string_view sql) {
  char *error = nullptr;
  const auto result = sqlite3_exec(database, std::string(sql).c_str(), nullptr,
                                   nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string detail =
        error == nullptr ? sqlite3_errmsg(database) : error;
    sqlite3_free(error);
    if (result == SQLITE_FULL || result == SQLITE_TOOBIG ||
        result == SQLITE_NOMEM) {
      throw CanonicalEmptySceneOwnerLimitError(detail);
    }
    if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
      throw CanonicalEmptySceneOwnerBusyError(detail);
    }
    if (result == SQLITE_CONSTRAINT) {
      throw CanonicalEmptySceneOwnerConflictError(detail);
    }
    if (result == SQLITE_CORRUPT || result == SQLITE_NOTADB ||
        result == SQLITE_SCHEMA) {
      throw CanonicalEmptySceneOwnerIntegrityError(detail);
    }
    throw CanonicalEmptySceneOwnerError(detail);
  }
}

class Statement final {
public:
  Statement(sqlite3 *database, const std::string_view sql)
      : database_(database) {
    Check(database_,
          sqlite3_prepare_v2(database_, std::string(sql).c_str(), -1,
                             &statement_, nullptr),
          "prepare SQLite statement");
  }

  ~Statement() {
    if (statement_ != nullptr) {
      sqlite3_finalize(statement_);
    }
  }

  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;

  void Bind(const int index, const std::string_view value) {
    Check(database_,
          sqlite3_bind_text(statement_, index, value.data(),
                            static_cast<int>(value.size()), SQLITE_TRANSIENT),
          "bind SQLite text");
  }

  void Bind(const int index, const std::int64_t value) {
    Check(database_, sqlite3_bind_int64(statement_, index, value),
          "bind SQLite integer");
  }

  void BindNull(const int index) {
    Check(database_, sqlite3_bind_null(statement_, index), "bind SQLite null");
  }

  [[nodiscard]] bool Step() {
    const auto result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    ThrowSqlite(database_, result, "step SQLite statement");
  }

  [[nodiscard]] std::string Text(const int column) const {
    if (sqlite3_column_type(statement_, column) != SQLITE_TEXT) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored logical-scene text has an invalid SQLite type");
    }
    const auto *value = sqlite3_column_text(statement_, column);
    if (value == nullptr) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored logical-scene text is null");
    }
    return {
        reinterpret_cast<const char *>(value),
        static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)),
    };
  }

  [[nodiscard]] std::int64_t Integer(const int column) const {
    if (sqlite3_column_type(statement_, column) != SQLITE_INTEGER) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored logical-scene integer has an invalid SQLite type");
    }
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] bool IsNull(const int column) const noexcept {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
  }

private:
  sqlite3 *database_;
  sqlite3_stmt *statement_{};
};

class Transaction final {
public:
  Transaction(sqlite3 *database, const std::string_view begin)
      : database_(database) {
    Exec(database_, begin);
  }

  ~Transaction() {
    if (!finished_) {
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
  }

  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

  void Commit() {
    Exec(database_, "COMMIT");
    finished_ = true;
  }

private:
  sqlite3 *database_;
  bool finished_{};
};

std::int64_t ScalarInteger(sqlite3 *database, const std::string_view sql) {
  Statement statement(database, sql);
  if (!statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "SQLite scalar row is missing");
  }
  const auto value = statement.Integer(0);
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "SQLite scalar query returned multiple rows");
  }
  return value;
}

std::string ScalarText(sqlite3 *database, const std::string_view sql) {
  Statement statement(database, sql);
  if (!statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "SQLite scalar text row is missing");
  }
  auto value = statement.Text(0);
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "SQLite scalar text query returned multiple rows");
  }
  return value;
}

std::string NormalizedSql(const std::string_view value) {
  std::istringstream input{std::string(value)};
  std::string word;
  std::string result;
  while (input >> word) {
    if (!result.empty()) {
      result.push_back(' ');
    }
    result += word;
  }
  return result;
}

using SchemaObjects =
    std::map<std::pair<std::string, std::string>, std::string>;

SchemaObjects ReadSchemaObjects(sqlite3 *database) {
  Statement statement(
      database, "SELECT type,name,sql FROM sqlite_master "
                "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name LIMIT 72");
  SchemaObjects objects;
  std::int64_t count = 0;
  while (statement.Step()) {
    ++count;
    if (count > kExpectedSchemaObjectCardinality) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene schema object cardinality exceeds the current bound");
    }
    objects.emplace(std::make_pair(statement.Text(0), statement.Text(1)),
                    NormalizedSql(statement.Text(2)));
  }
  return objects;
}

SchemaObjects BuildExpectedSchemaObjects(const std::int64_t schema_version) {
  sqlite3 *database = nullptr;
  const auto result = sqlite3_open_v2(
      ":memory:", &database,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
      nullptr);
  if (result != SQLITE_OK) {
    const std::string detail =
        database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
    if (database != nullptr) {
      sqlite3_close(database);
    }
    throw CanonicalEmptySceneOwnerError(
        "cannot open in-memory scene schema validator: " + detail);
  }
  try {
    Exec(database, detail::kEmbeddedCanonicalEmptySceneSchema);
    if (schema_version >= kLogicalSceneTransactionStoreSchemaVersion) {
      Exec(database, detail::kEmbeddedLogicalSceneTransactionMigration);
    }
    if (schema_version >= kNativeProjectionHandoffStoreSchemaVersion) {
      Exec(database, detail::kEmbeddedNativeProjectionHandoffMigration);
    }
    if (schema_version >= kLogicalSceneStoreSchemaVersion) {
      Exec(database, detail::kEmbeddedBoundedHistoryMigration);
    }
    auto objects = ReadSchemaObjects(database);
    const auto expected_count =
        schema_version == kCanonicalEmptySceneStoreSchemaVersion
            ? kExpectedBaseSchemaObjectCardinality
        : schema_version == kLogicalSceneTransactionStoreSchemaVersion
            ? kExpectedTransactionSchemaObjectCardinality
        : schema_version == kNativeProjectionHandoffStoreSchemaVersion
            ? kExpectedNativeHandoffSchemaObjectCardinality
            : kExpectedSchemaObjectCardinality;
    if (objects.size() != static_cast<std::size_t>(expected_count)) {
      throw CanonicalEmptySceneOwnerError(
          "embedded logical-scene schema cardinality is invalid");
    }
    sqlite3_close(database);
    return objects;
  } catch (...) {
    sqlite3_close(database);
    throw;
  }
}

const SchemaObjects &ExpectedBaseSchemaObjects() {
  static const SchemaObjects expected =
      BuildExpectedSchemaObjects(kCanonicalEmptySceneStoreSchemaVersion);
  return expected;
}

const SchemaObjects &ExpectedTransactionSchemaObjects() {
  static const SchemaObjects expected =
      BuildExpectedSchemaObjects(kLogicalSceneTransactionStoreSchemaVersion);
  return expected;
}

const SchemaObjects &ExpectedNativeHandoffSchemaObjects() {
  static const SchemaObjects expected =
      BuildExpectedSchemaObjects(kNativeProjectionHandoffStoreSchemaVersion);
  return expected;
}

const SchemaObjects &ExpectedSchemaObjects() {
  static const SchemaObjects expected =
      BuildExpectedSchemaObjects(kLogicalSceneStoreSchemaVersion);
  return expected;
}

void ValidateBoundedIdentity(const std::string_view value,
                             const std::string_view label) {
  if (value.empty() || value.size() > kMaximumSceneStoreIdentifierBytes ||
      std::any_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x21U || byte > 0x7EU;
      })) {
    throw std::invalid_argument(std::string(label) +
                                " must be bounded non-empty printable ASCII");
  }
}

void ValidateDigest(const std::string_view value,
                    const std::string_view label) {
  if (!core::IsExactLowercaseSha256Digest(value)) {
    throw std::invalid_argument(std::string(label) +
                                " must be an exact lowercase SHA-256 digest");
  }
}

RequestOwnerEpoch
CompactedRequestOwnerEpoch(const SceneId &scene_id,
                           const std::uint64_t request_epoch_generation) {
  if (scene_id.empty() || request_epoch_generation <= 1U ||
      request_epoch_generation > kMaximumSafeIntegral) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot derive a bounded logical-scene request owner epoch");
  }
  const auto epoch_digest = manyforge::content_identity::sha256_digest(
      "manyforge.scene.request-owner-epoch.v1\n" + scene_id.value() + "\n" +
      std::to_string(request_epoch_generation));
  RequestOwnerEpoch epoch("scene-request:" + epoch_digest);
  ValidateBoundedIdentity(epoch.value(), "compacted request owner epoch");
  return epoch;
}

std::int64_t CheckedSafeCounter(const std::uint64_t value,
                                const std::string_view label) {
  if (value == 0U || value > kMaximumSafeIntegral) {
    throw std::invalid_argument(
        std::string(label) +
        " must fit the nonzero RFC-8785 safe-integral range");
  }
  return static_cast<std::int64_t>(value);
}

void ValidateFixedQuery(const ProjectionBootstrapPrerequisiteQueryDTO &query) {
  if (!query.valid()) {
    throw std::invalid_argument(
        "canonical-empty scene prerequisite query is invalid");
  }
  ValidateBoundedIdentity(query.cell_id.value(), "cell_id");
  ValidateDigest(query.canonical_empty_retained_bundle_id.value(),
                 "canonical_empty_retained_bundle_id");
  ValidateDigest(query.canonical_empty_graph_revision.value(),
                 "canonical_empty_graph_revision");
}

struct AdmittedBootstrapAuthority final {
  ProjectionBootstrapPrerequisiteQueryDTO query;
  ContentDigest admission_digest;
  RetainedBundleDTO retained_bundle;
};

AdmittedBootstrapAuthority AuthorityFromAdmission(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  const auto &retained = admission.retained_bundle();
  const auto &bundle = retained.value();
  if (admission.api_version() !=
          deployment::kCanonicalEmptyAdmissionApiVersion ||
      admission.canonical_bytes().empty() ||
      admission.canonical_bytes().size() >
          deployment::kMaxCanonicalEmptyAdmissionBytes ||
      !deployment::verify_retained_bundle_identity(retained) ||
      retained.retained_bundle_id() != admission.retained_bundle_id() ||
      bundle.retained_bundle_id != admission.retained_bundle_id() ||
      bundle.cell_graph.cell_id != admission.cell_id() ||
      bundle.cell_graph.graph_revision != admission.graph_revision()) {
    throw std::invalid_argument(
        "scene owner requires one complete sealed canonical-empty "
        "admission");
  }
  ValidateDigest(admission.admission_digest().value(),
                 "canonical-empty admission digest");
  ProjectionBootstrapPrerequisiteQueryDTO query{
      admission.cell_id(),
      admission.retained_bundle_id(),
      admission.graph_revision(),
  };
  ValidateFixedQuery(query);
  return {
      std::move(query),
      admission.admission_digest(),
      bundle,
  };
}

std::string BaseSchemaArtifactDigest() {
  return manyforge::content_identity::sha256_digest(
      detail::kEmbeddedCanonicalEmptySceneSchema);
}

std::string TransactionMigrationArtifactDigest() {
  return manyforge::content_identity::sha256_digest(
      detail::kEmbeddedLogicalSceneTransactionMigration);
}

std::string NativeHandoffMigrationArtifactDigest() {
  return manyforge::content_identity::sha256_digest(
      detail::kEmbeddedNativeProjectionHandoffMigration);
}

std::string BoundedHistoryMigrationArtifactDigest() {
  return manyforge::content_identity::sha256_digest(
      detail::kEmbeddedBoundedHistoryMigration);
}

std::string CurrentSchemaArtifactDigest() {
  return manyforge::content_identity::sha256_digest(
      std::string(detail::kEmbeddedCanonicalEmptySceneSchema) + "\n--v4--\n" +
      std::string(detail::kEmbeddedLogicalSceneTransactionMigration) +
      "\n--v5--\n" +
      std::string(detail::kEmbeddedNativeProjectionHandoffMigration) +
      "\n--v6--\n" + std::string(detail::kEmbeddedBoundedHistoryMigration));
}

struct AcceptedState final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionRevisionDTO target;
  LogicalSceneSnapshotDTO snapshot;
};

struct NamedApplyStageState final {
  std::uint64_t lineage_generation;
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration stage_generation;
  ProjectionRevisionDTO predecessor;
  ProjectionRevisionDTO candidate;
  bool activated{};
  bool discarded{};
  std::optional<ActiveBundlePublicationReceiptDTO> finalization_receipt;
  std::vector<core::ProjectionTransitionRowDTO> native_transition_rows;
  std::optional<LogicalSceneSnapshotDTO> frozen_snapshot;
  bool stage_completed{true};
  bool activation_selected{};
};

struct NativeRuntimeRecoveryState final {
  NativeProjectionRecoveryGeneration recovery_generation;
  std::uint64_t structural_lineage_generation;
  ProjectionRevisionDTO target;
  std::vector<core::ProjectionNativeRuntimeBindingDTO> bindings;
  std::optional<std::uint64_t> recovered_transaction_sequence;
  std::optional<std::vector<core::ProjectionHealthDTO>> completion_health;
};

struct DurableState final {
  std::optional<AcceptedState> accepted;
  bool rebuilt{};
  std::optional<ActiveBundlePublicationReceiptDTO> finalization_receipt;
  std::vector<NamedApplyStageState> named_apply_stages;
  std::optional<ProjectionRevisionDTO> active_projection;
  std::optional<LogicalSceneSnapshotDTO> active_scene_snapshot;
  std::vector<NativeRuntimeRecoveryState> native_runtime_recoveries;
  bool projection_desynchronized{};
  bool unresolved_scene_transaction{};
  std::optional<std::uint64_t> latest_uncertain_transaction_sequence;

  [[nodiscard]] const NamedApplyStageState *
  FindNamedApply(const ApplyId &apply_id,
                 const ApplyGeneration apply_generation) const noexcept {
    const auto found =
        std::find_if(named_apply_stages.begin(), named_apply_stages.end(),
                     [&](const NamedApplyStageState &stage) {
                       return stage.apply_id == apply_id &&
                              stage.apply_generation == apply_generation;
                     });
    return found == named_apply_stages.end() ? nullptr : &*found;
  }

  [[nodiscard]] const NamedApplyStageState *OpenNamedApply() const noexcept {
    if (named_apply_stages.empty()) {
      return nullptr;
    }
    const auto &stage = named_apply_stages.back();
    return !stage.discarded && !stage.finalization_receipt.has_value()
               ? &stage
               : nullptr;
  }

  [[nodiscard]] const NamedApplyStageState *
  ActiveStructuralStage() const noexcept {
    const auto found =
        std::find_if(named_apply_stages.rbegin(), named_apply_stages.rend(),
                     [](const NamedApplyStageState &stage) {
                       return stage.finalization_receipt.has_value();
                     });
    return found == named_apply_stages.rend() ? nullptr : &*found;
  }

  [[nodiscard]] const NativeRuntimeRecoveryState *
  LatestActiveRuntimeRecovery() const noexcept {
    const auto *active_stage = ActiveStructuralStage();
    if (active_stage == nullptr) {
      return nullptr;
    }
    const auto found = std::find_if(
        native_runtime_recoveries.rbegin(), native_runtime_recoveries.rend(),
        [&](const NativeRuntimeRecoveryState &recovery) {
          return recovery.structural_lineage_generation ==
                 active_stage->lineage_generation;
        });
    return found == native_runtime_recoveries.rend() ? nullptr : &*found;
  }

  [[nodiscard]] CanonicalEmptySceneBootstrapPhase phase() const noexcept {
    if (!named_apply_stages.empty()) {
      const auto &stage = named_apply_stages.back();
      if (stage.finalization_receipt.has_value()) {
        return CanonicalEmptySceneBootstrapPhase::kNamedApplyFinalized;
      }
      if (stage.discarded) {
        return CanonicalEmptySceneBootstrapPhase::kNamedApplyDiscarded;
      }
      if (stage.activated) {
        return CanonicalEmptySceneBootstrapPhase::kNamedApplyActivated;
      }
      return CanonicalEmptySceneBootstrapPhase::kNamedApplyStaged;
    }
    if (finalization_receipt.has_value()) {
      return CanonicalEmptySceneBootstrapPhase::kFinalized;
    }
    if (rebuilt) {
      return CanonicalEmptySceneBootstrapPhase::kReadyToPublish;
    }
    if (accepted.has_value()) {
      return CanonicalEmptySceneBootstrapPhase::kAccepted;
    }
    return CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced;
  }
};

[[nodiscard]] CanonicalEmptySceneAccessStatus
AccessStatus(const DurableState &state);

/// Returns the exact native set selected by the latest finalized structural
/// transition. Discarded or still-open stages never alter the active set.
/// Canonical-empty bootstrap currently has an intentionally empty native set.
[[nodiscard]] std::vector<core::ProjectionNativeRuntimeBindingDTO>
ActiveNativeRuntimeBindings(const DurableState &state) {
  const auto *finalized = state.ActiveStructuralStage();
  if (finalized == nullptr) {
    return {};
  }
  std::vector<core::ProjectionNativeRuntimeBindingDTO> bindings;
  for (const auto &row : finalized->native_transition_rows) {
    if (row.disposition != core::ProjectionTransitionDisposition::kRemoved) {
      bindings.push_back(row.native_runtime_binding);
    }
  }
  for (const auto &recovery : state.native_runtime_recoveries) {
    if (recovery.structural_lineage_generation ==
            finalized->lineage_generation &&
        recovery.completion_health.has_value()) {
      bindings = recovery.bindings;
    }
  }
  if (!std::is_sorted(bindings.begin(), bindings.end(),
                      core::CanonicalProjectionNativeBindingLess) ||
      std::adjacent_find(bindings.begin(), bindings.end()) != bindings.end()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "finalized native projection set is not canonical and unique");
  }
  return bindings;
}

struct StoredSceneOutcome final {
  std::string request_owner_epoch;
  RequestId request_id;
  ContentDigest request_digest;
  std::optional<std::uint64_t> transaction_sequence;
  bool success;
  SceneProjectionCommitStatusDTO projection_status;
  std::size_t attempted_projection_count;
  std::optional<ErrorDTO> error;
};

struct StoredSceneIntent final {
  std::uint64_t transaction_sequence;
  std::string request_owner_epoch;
  RequestId request_id;
  ContentDigest request_digest;
  std::string request_json;
  ProjectionRevisionDTO prior;
  ProjectionRevisionDTO proposed;
  std::string resolved_operation_json;
  std::optional<ContentDigest> resolution_evidence_digest;
  LogicalSceneSnapshotDTO candidate_snapshot;
  SceneUpdateStatusDTO update_status;
  std::size_t required_projection_count;
};

SceneProjectionCommitStatusDTO
DecodeProjectionStatus(std::string_view value);

[[nodiscard]] bool IsAuthoredSceneMutation(
    const SceneTransactionRequestDTO &request) {
  if (std::holds_alternative<core::AddObjectRequestDTO>(request.operation) ||
      std::holds_alternative<core::UpsertObjectRequestDTO>(request.operation) ||
      std::holds_alternative<core::RemoveObjectRequestDTO>(request.operation) ||
      std::holds_alternative<core::SetCollisionPolicyRequestDTO>(
          request.operation)) {
    return true;
  }
  if (const auto *update =
          std::get_if<core::UpdateObjectPoseRequestDTO>(&request.operation)) {
    return !update->provider_evidence.has_value();
  }
  return false;
}

[[nodiscard]] core::CellAdmissionClaimIntentDTO AuthoredSceneClaimIntent(
    const SceneTransactionRequestDTO &request,
    const ContentDigest &request_digest) {
  if (!request.expected_admission_generation.has_value()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "authored scene mutation lacks its admission generation");
  }
  core::CellAdmissionClaimIntentDTO intent{
      core::CellAdmissionClaimKindDTO::kAuthoredScene,
      core::CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner,
      request.request_context.request_owner_epoch,
      request.request_context.request_id.value(),
      request.request_context.request_id,
      request_digest,
      *request.expected_admission_generation,
      {},
      std::nullopt,
      std::nullopt,
  };
  if (!core::ValidateCellAdmissionClaimIntent(intent).ok()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "authored scene mutation formed an invalid admission intent");
  }
  return intent;
}

[[nodiscard]] core::CellAdmissionClaimDTO RequireAuthoredSceneClaim(
    core::IKernelCellAdmissionPort &admission,
    const core::CellAdmissionClaimIntentDTO &intent) {
  const auto receipt = admission.GetCellAdmissionRecoveryReceipt(
      {intent, std::nullopt, std::nullopt});
  if (!receipt.ok()) {
    ThrowCellAdmissionFailure("authored scene admission lookup failed",
                              receipt.error());
  }
  if (receipt.value().status ==
          core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
      receipt.value().matched_claim.has_value()) {
    return *receipt.value().matched_claim;
  }
  if (receipt.value().status !=
      core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown) {
    throw CanonicalEmptySceneOwnerConflictError(
        "authored scene admission identity conflicts or is already settled");
  }
  const auto acquired = admission.AcquireCellAdmissionClaim(intent);
  if (!acquired.ok()) {
    ThrowCellAdmissionFailure("authored scene admission was rejected",
                              acquired.error());
  }
  return acquired.value();
}

void ReleaseAuthoredSceneClaim(
    core::IKernelCellAdmissionPort &admission,
    const core::CellAdmissionClaimDTO &claim,
    const ContentDigest &terminal_evidence,
    const core::CellAdmissionSettlementKindDTO settlement_kind) {
  const core::CellAdmissionClaimReleaseDTO release{
      claim.intent.owner_role,
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      {settlement_kind, terminal_evidence},
  };
  const auto released = admission.ReleaseCellAdmissionClaim(release);
  if (!released.ok()) {
    ThrowCellAdmissionFailure("authored scene settlement was rejected",
                              released.error());
  }
}

void SettleAuthoredSceneClaimIfHeld(
    core::IKernelCellAdmissionPort &admission,
    const SceneTransactionRequestDTO &request,
    const ContentDigest &request_digest, const bool success,
    const SceneProjectionCommitStatusDTO projection_status) {
  if (!IsAuthoredSceneMutation(request) ||
      !request.expected_admission_generation.has_value() ||
      (!success && projection_status !=
                       SceneProjectionCommitStatusDTO::kRejectedRollbackProven &&
       projection_status != SceneProjectionCommitStatusDTO::kNotAttempted)) {
    return;
  }
  const auto intent = AuthoredSceneClaimIntent(request, request_digest);
  const auto receipt = admission.GetCellAdmissionRecoveryReceipt(
      {intent, std::nullopt, std::nullopt});
  if (!receipt.ok()) {
    ThrowCellAdmissionFailure("authored scene recovery lookup failed",
                              receipt.error());
  }
  if (receipt.value().status ==
      core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown) {
    return;
  }
  if (receipt.value().status ==
      core::CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact) {
    return;
  }
  if (receipt.value().status !=
          core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
      !receipt.value().matched_claim.has_value()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "authored scene recovery claim is conflicting");
  }
  ReleaseAuthoredSceneClaim(
      admission, *receipt.value().matched_claim, request_digest,
      success ? core::CellAdmissionSettlementKindDTO::kOwnerCommit
              : core::CellAdmissionSettlementKindDTO::kProvenNoEffect);
}

void SettleRecoveredAuthoredSceneClaims(
    sqlite3 *database, core::IKernelCellAdmissionPort &admission) {
  Statement rows(
      database,
      "SELECT i.request_json,i.request_digest,o.success,o.projection_status "
      "FROM logical_scene_transaction_intents i "
      "JOIN logical_scene_request_outcomes o "
      "ON o.request_owner_epoch=i.request_owner_epoch "
      "AND o.request_id=i.request_id ORDER BY i.transaction_sequence");
  while (rows.Step()) {
    const auto request =
        logical_scene_codec_internal::DecodeRequest(rows.Text(0));
    const auto success = rows.Integer(2) == 1;
    SettleAuthoredSceneClaimIfHeld(
        admission, request, ContentDigest(rows.Text(1)), success,
        DecodeProjectionStatus(rows.Text(3)));
  }
}

struct SceneTransactionStoreState final {
  ProjectionRevisionDTO active_projection;
  LogicalSceneSnapshotDTO active_snapshot;
  RequestOwnerEpoch request_owner_epoch;
  std::uint64_t next_transaction_sequence;
  std::uint64_t next_owner_event_sequence;
  std::size_t resident_transaction_count;
  bool projection_desynchronized;
  bool unresolved_native_attempt;
  std::optional<std::uint64_t> unresolved_transaction_sequence;
  std::optional<std::uint64_t> latest_uncertain_transaction_sequence;
};

/// Process-local acceleration for the ordinary scene-commit path.
///
/// Startup and every structural/recovery mutation still reconstruct and
/// validate the complete durable history. Between those boundaries this owner
/// is the sole writer and holds `Impl::mutex_`, so replaying all resident
/// transaction JSON before every edit is both redundant and quadratic. The
/// SQLite `data_version` value fences writes made through another connection;
/// any such change discards this cache and forces a complete reconstruction.
struct SceneCommitStateCache final {
  DurableState durable_state;
  std::optional<SceneTransactionStoreState> transaction_state;
  std::int64_t external_data_version{};
};

struct BoundedHistoryState final {
  std::uint64_t compaction_generation{};
  std::uint64_t request_epoch_generation{};
  std::optional<RequestOwnerEpoch> request_owner_epoch;
  std::uint64_t compacted_through_transaction_sequence{};
  std::uint64_t compacted_through_owner_event_sequence{};
  std::size_t compacted_named_apply_stage_count{};
  std::uint64_t recovered_through_transaction_sequence{};
  std::optional<ProjectionRevisionDTO> checkpoint_projection;
  std::optional<LogicalSceneSnapshotDTO> checkpoint_snapshot;
  std::uint64_t native_recovery_generation_watermark{};
};

[[nodiscard]] BoundedHistoryState ReadBoundedHistoryState(sqlite3 *database) {
  if (ScalarInteger(database, "PRAGMA user_version") <
      kLogicalSceneStoreSchemaVersion) {
    return {};
  }
  Statement statement(
      database, "SELECT compaction_generation,request_epoch_generation,"
                "request_owner_epoch,compacted_through_transaction_sequence,"
                "compacted_through_owner_event_sequence,"
                "compacted_named_apply_stage_count,"
                "recovered_through_transaction_sequence,"
                "checkpoint_retained_bundle_id,"
                "checkpoint_graph_revision,checkpoint_scene_id,"
                "checkpoint_scene_revision_id,checkpoint_scene_content_hash,"
                "checkpoint_snapshot_json,native_recovery_generation_watermark,"
                "compaction_in_progress FROM logical_scene_history_state "
                "ORDER BY singleton LIMIT 2");
  if (!statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "bounded logical-scene history state is missing");
  }
  const auto nonnegative = [&](const int column, const std::string_view label) {
    const auto value = statement.Integer(column);
    if (value < 0 || static_cast<std::uint64_t>(value) > kMaximumSafeIntegral) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "bounded logical-scene " + std::string(label) + " is invalid");
    }
    return static_cast<std::uint64_t>(value);
  };
  BoundedHistoryState state;
  state.compaction_generation = nonnegative(0, "compaction generation");
  state.request_epoch_generation = nonnegative(1, "request epoch generation");
  if (!statement.IsNull(2)) {
    ValidateBoundedIdentity(statement.Text(2), "stored request owner epoch");
    state.request_owner_epoch = RequestOwnerEpoch(statement.Text(2));
  }
  state.compacted_through_transaction_sequence =
      nonnegative(3, "transaction watermark");
  state.compacted_through_owner_event_sequence =
      nonnegative(4, "owner-event watermark");
  const auto stage_count = nonnegative(5, "named-Apply stage watermark");
  if (stage_count > kMaximumNamedApplySceneStages) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "bounded logical-scene named-Apply watermark exceeds its bound");
  }
  state.compacted_named_apply_stage_count =
      static_cast<std::size_t>(stage_count);
  state.recovered_through_transaction_sequence =
      nonnegative(6, "recovered transaction watermark");
  const bool checkpoint_present = !statement.IsNull(7);
  for (int column = 8; column <= 12; ++column) {
    if (statement.IsNull(column) == checkpoint_present) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "bounded logical-scene checkpoint has partial presence");
    }
  }
  if (checkpoint_present) {
    const auto revision_id = statement.Integer(10);
    if (revision_id <= 0) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "bounded logical-scene checkpoint revision is invalid");
    }
    ProjectionRevisionDTO projection{
        RetainedBundleId(statement.Text(7)), GraphRevision(statement.Text(8)),
        SceneRevision{SceneId(statement.Text(9)),
                      static_cast<std::uint64_t>(revision_id),
                      ContentDigest(statement.Text(11))}};
    auto snapshot =
        logical_scene_codec_internal::DecodeSnapshot(statement.Text(12));
    if (!projection.valid() || snapshot.revision != projection.scene_revision) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "bounded logical-scene checkpoint content is inconsistent");
    }
    state.checkpoint_projection = std::move(projection);
    state.checkpoint_snapshot = std::move(snapshot);
  }
  state.native_recovery_generation_watermark =
      nonnegative(13, "native recovery generation watermark");
  if (statement.Integer(14) != 0 || statement.Step() ||
      (state.request_epoch_generation == 0U) !=
          !state.request_owner_epoch.has_value() ||
      (state.compaction_generation == 0U) !=
          !state.checkpoint_projection.has_value() ||
      state.checkpoint_projection.has_value() !=
          state.checkpoint_snapshot.has_value()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "bounded logical-scene history state violates its closed shape");
  }
  if (state.compaction_generation == 0U) {
    if (state.request_epoch_generation != 0U) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "uncompacted logical-scene history carries a request epoch");
    }
  } else if (state.compaction_generation >= kMaximumSafeIntegral ||
             state.request_epoch_generation !=
                 state.compaction_generation + 1U ||
             *state.request_owner_epoch !=
                 CompactedRequestOwnerEpoch(
                     state.checkpoint_projection->scene_revision.scene_id,
                     state.request_epoch_generation)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "compacted logical-scene request epoch is not deterministic");
  }
  return state;
}

struct StoredSceneRecord final {
  StoredSceneIntent intent;
  std::size_t projection_attempt_count;
  std::size_t projection_result_count;
  std::optional<StoredSceneOutcome> outcome;
};

SceneProjectionCommitStatusDTO
DecodeProjectionStatus(const std::string_view value) {
  if (value == "not_attempted") {
    return SceneProjectionCommitStatusDTO::kNotAttempted;
  }
  if (value == "committed") {
    return SceneProjectionCommitStatusDTO::kCommitted;
  }
  if (value == "rejected_rollback_proven") {
    return SceneProjectionCommitStatusDTO::kRejectedRollbackProven;
  }
  if (value == "failed_projection_uncertain") {
    return SceneProjectionCommitStatusDTO::kFailedProjectionUncertain;
  }
  throw CanonicalEmptySceneOwnerIntegrityError(
      "stored scene projection status is invalid");
}

SceneUpdateStatusDTO DecodeUpdateStatus(const std::string_view value) {
  if (value == "geometry_changed") {
    return SceneUpdateStatusDTO::kGeometryChanged;
  }
  if (value == "freshness_only") {
    return SceneUpdateStatusDTO::kFreshnessOnly;
  }
  if (value == "no_op") {
    return SceneUpdateStatusDTO::kNoOp;
  }
  throw CanonicalEmptySceneOwnerIntegrityError(
      "stored logical-scene update status is invalid");
}

ProjectionRevisionDTO ReadProjectionColumns(const Statement &statement,
                                            const int offset) {
  const auto revision_id = statement.Integer(offset + 3);
  if (revision_id <= 0) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored logical-scene projection revision is invalid");
  }
  ProjectionRevisionDTO projection{
      RetainedBundleId(statement.Text(offset)),
      GraphRevision(statement.Text(offset + 1)),
      SceneRevision{
          SceneId(statement.Text(offset + 2)),
          static_cast<std::uint64_t>(revision_id),
          ContentDigest(statement.Text(offset + 4)),
      },
  };
  if (!projection.valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored logical-scene projection identity is invalid");
  }
  return projection;
}

std::optional<StoredSceneOutcome>
ReadOutcomeForRequest(sqlite3 *database,
                      const std::string_view request_owner_epoch,
                      const RequestId &request_id) {
  Statement statement(database,
                      "SELECT request_owner_epoch,request_id,request_digest,"
                      "transaction_sequence,success,projection_status,"
                      "attempted_projection_count,error_code,error_message,"
                      "error_details_json FROM logical_scene_request_outcomes "
                      "WHERE request_owner_epoch=? AND request_id=? LIMIT 2");
  statement.Bind(1, request_owner_epoch);
  statement.Bind(2, request_id.value());
  if (!statement.Step()) {
    return std::nullopt;
  }
  StoredSceneOutcome outcome{
      statement.Text(0),
      RequestId(statement.Text(1)),
      ContentDigest(statement.Text(2)),
      statement.IsNull(3)
          ? std::nullopt
          : std::optional<std::uint64_t>(
                static_cast<std::uint64_t>(statement.Integer(3))),
      statement.Integer(4) == 1,
      DecodeProjectionStatus(statement.Text(5)),
      static_cast<std::size_t>(statement.Integer(6)),
      std::nullopt,
  };
  if (outcome.transaction_sequence.has_value() &&
      *outcome.transaction_sequence == 0U) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored scene outcome has a zero transaction sequence");
  }
  if (outcome.success) {
    if (!statement.IsNull(7) || !statement.IsNull(8) || !statement.IsNull(9)) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "successful scene outcome carries an error");
    }
  } else {
    if (statement.IsNull(7) || statement.IsNull(8) || statement.IsNull(9)) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "failed scene outcome lacks its error");
    }
    outcome.error = ErrorDTO{
        statement.Text(7),
        statement.Text(8),
        logical_scene_codec_internal::DecodeErrorDetails(statement.Text(9)),
    };
  }
  if (statement.Step() ||
      !core::IsExactLowercaseSha256Digest(outcome.request_digest.value()) ||
      outcome.request_owner_epoch != request_owner_epoch ||
      outcome.request_id != request_id ||
      (outcome.success && (outcome.projection_status !=
                               SceneProjectionCommitStatusDTO::kCommitted ||
                           !outcome.transaction_sequence.has_value() ||
                           outcome.error.has_value())) ||
      (!outcome.success && outcome.projection_status ==
                               SceneProjectionCommitStatusDTO::kCommitted)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored scene outcome violates its closed result shape");
  }
  return outcome;
}

std::optional<StoredSceneOutcome>
ReadOutcomeForSequence(sqlite3 *database,
                       const std::uint64_t transaction_sequence) {
  Statement key(database, "SELECT request_owner_epoch,request_id FROM "
                          "logical_scene_transaction_intents WHERE "
                          "transaction_sequence=? LIMIT 2");
  key.Bind(1, CheckedSafeCounter(transaction_sequence, "transaction_sequence"));
  if (!key.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "scene outcome lookup has no matching intent");
  }
  const auto epoch = key.Text(0);
  const RequestId request_id(key.Text(1));
  if (key.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "scene intent sequence is not unique");
  }
  return ReadOutcomeForRequest(database, epoch, request_id);
}

struct ProjectionEvidenceState final {
  std::size_t attempt_count{};
  std::size_t result_count{};
  bool all_results_committed{true};
  bool all_results_rollback_proven{true};
  bool any_result_committed{};
  bool any_result_uncertain{};
};

ProjectionEvidenceState
ReadProjectionEvidenceState(sqlite3 *database,
                            const std::uint64_t transaction_sequence,
                            const ProjectionRevisionDTO &proposed) {
  ProjectionEvidenceState evidence;
  std::optional<core::ProjectionNativeRuntimeBindingDTO> previous_binding;
  Statement attempts(
      database,
      "SELECT projection_ordinal,provider_binding_id,"
      "provider_binding_revision,runtime_unit_id,runtime_epoch FROM "
      "logical_scene_projection_attempts WHERE transaction_sequence=? "
      "ORDER BY projection_ordinal LIMIT 257");
  attempts.Bind(
      1, CheckedSafeCounter(transaction_sequence, "transaction_sequence"));
  while (attempts.Step()) {
    if (attempts.Integer(0) !=
        static_cast<std::int64_t>(evidence.attempt_count)) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene projection attempt ordinals are not contiguous");
    }
    core::ProjectionNativeRuntimeBindingDTO binding{
        core::CapabilityProviderBindingId(attempts.Text(1)),
        core::CapabilityProviderBindingRevision(attempts.Text(2)),
        core::RuntimeUnitId(attempts.Text(3)),
        core::RuntimeEpoch(attempts.Text(4)),
    };
    if (!binding.valid() || (previous_binding.has_value() &&
                             !core::CanonicalProjectionNativeBindingLess(
                                 *previous_binding, binding))) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene projection attempt bindings are invalid");
    }
    previous_binding = std::move(binding);
    ++evidence.attempt_count;
  }

  Statement results(
      database,
      "SELECT projection_ordinal,result_state,"
      "acknowledged_retained_bundle_id,acknowledged_graph_revision,"
      "acknowledged_scene_id,acknowledged_scene_revision_id,"
      "acknowledged_scene_content_hash,rollback_proven,"
      "resulting_health_state,error_code,error_message,error_details_json "
      "FROM logical_scene_projection_results WHERE transaction_sequence=? "
      "ORDER BY projection_ordinal LIMIT 257");
  results.Bind(
      1, CheckedSafeCounter(transaction_sequence, "transaction_sequence"));
  while (results.Step()) {
    if (results.Integer(0) !=
            static_cast<std::int64_t>(evidence.result_count) ||
        evidence.result_count >= evidence.attempt_count) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene projection result ordinals are invalid");
    }
    const auto state = results.Text(1);
    const bool acknowledgement_present = !results.IsNull(2);
    for (int column = 2; column <= 6; ++column) {
      if (results.IsNull(column) == acknowledgement_present) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "stored projection acknowledgement has partial presence");
      }
    }
    std::optional<ProjectionRevisionDTO> acknowledgement;
    if (acknowledgement_present) {
      acknowledgement = ReadProjectionColumns(results, 2);
    }
    const bool rollback_proven = results.Integer(7) == 1;
    const auto health_state = results.Text(8);
    const bool error_present = !results.IsNull(9);
    if (results.IsNull(10) != !error_present ||
        results.IsNull(11) != !error_present) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored projection result error has partial presence");
    }
    if (error_present) {
      static_cast<void>(
          logical_scene_codec_internal::DecodeErrorDetails(results.Text(11)));
    }
    if (state == "committed") {
      if (!acknowledgement.has_value() || *acknowledgement != proposed ||
          rollback_proven || health_state != "healthy" || error_present) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "stored committed projection result is contradictory");
      }
      evidence.any_result_committed = true;
      evidence.all_results_rollback_proven = false;
    } else if (state == "rejected_rollback_proven") {
      if (!rollback_proven || health_state != "healthy" || !error_present) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "stored rollback-proven projection result is contradictory");
      }
      evidence.all_results_committed = false;
    } else if (state == "failed_projection_uncertain") {
      if (rollback_proven ||
          (health_state != "desynchronized" && health_state != "unavailable") ||
          !error_present) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "stored uncertain projection result is contradictory");
      }
      evidence.all_results_committed = false;
      evidence.all_results_rollback_proven = false;
      evidence.any_result_uncertain = true;
    } else {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored projection result state is invalid");
    }
    ++evidence.result_count;
  }
  if (evidence.result_count == 0U) {
    evidence.all_results_committed = false;
    evidence.all_results_rollback_proven = false;
  }
  return evidence;
}

SceneTransactionStoreState
ReadSceneTransactionStoreState(sqlite3 *database,
                               const DurableState &durable_state) {
  if (!durable_state.accepted.has_value() ||
      !durable_state.finalization_receipt.has_value()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene transaction history requires finalized bootstrap");
  }
  const auto &bootstrap_projection = durable_state.accepted->target;
  const auto &bootstrap_snapshot = durable_state.accepted->snapshot;
  const auto history = ReadBoundedHistoryState(database);
  if (history.compacted_named_apply_stage_count >
      durable_state.named_apply_stages.size()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene checkpoint names absent structural history");
  }
  ProjectionRevisionDTO replay_projection = bootstrap_projection;
  LogicalSceneSnapshotDTO replay_snapshot = bootstrap_snapshot;
  if (history.checkpoint_projection.has_value()) {
    replay_projection = *history.checkpoint_projection;
    replay_snapshot = *history.checkpoint_snapshot;
    if (replay_snapshot.cell_id != bootstrap_snapshot.cell_id ||
        replay_snapshot.revision.scene_id !=
            bootstrap_snapshot.revision.scene_id ||
        replay_snapshot.transaction_sequence >
            history.compacted_through_transaction_sequence) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene checkpoint is outside the accepted lineage");
    }
  }
  const RequestOwnerEpoch request_owner_epoch =
      history.request_owner_epoch.value_or(
          RequestOwnerEpoch(bootstrap_snapshot.revision.scene_id.value()));
  if (request_owner_epoch.empty() ||
      history.compacted_through_transaction_sequence >= kMaximumSafeIntegral ||
      history.compacted_through_owner_event_sequence >= kMaximumSafeIntegral) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene history watermarks cannot admit another record");
  }
  SceneTransactionStoreState state{
      replay_projection,
      replay_snapshot,
      request_owner_epoch,
      history.compacted_through_transaction_sequence + 1U,
      history.compacted_through_owner_event_sequence + 1U,
      0U,
      false,
      false,
      std::nullopt,
      std::nullopt,
  };
  std::vector<StoredSceneRecord> records;
  Statement statement(
      database,
      "SELECT transaction_sequence,request_owner_epoch,request_id,"
      "request_digest,request_json,prior_retained_bundle_id,"
      "prior_graph_revision,prior_scene_id,prior_scene_revision_id,"
      "prior_scene_content_hash,proposed_retained_bundle_id,"
      "proposed_graph_revision,proposed_scene_id,"
      "proposed_scene_revision_id,proposed_scene_content_hash,"
      "resolved_operation_json,resolution_evidence_digest,"
      "candidate_snapshot_json,update_status,required_projection_count "
      "FROM logical_scene_transaction_intents WHERE transaction_sequence>? "
      "ORDER BY transaction_sequence LIMIT 257");
  statement.Bind(1, static_cast<std::int64_t>(
                        history.compacted_through_transaction_sequence));
  std::uint64_t expected_sequence = state.next_transaction_sequence;
  while (statement.Step()) {
    if (!records.empty() &&
        (!records.back().outcome.has_value() ||
         (records.back().outcome->projection_status ==
              SceneProjectionCommitStatusDTO::kFailedProjectionUncertain &&
          records.back().intent.transaction_sequence >
              history.recovered_through_transaction_sequence))) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene history continues after an unresolved or "
          "desynchronized transaction");
    }
    const auto sequence_value = statement.Integer(0);
    if (sequence_value <= 0 ||
        static_cast<std::uint64_t>(sequence_value) != expected_sequence) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene transaction sequence is not contiguous");
    }
    StoredSceneIntent intent{
        expected_sequence,
        statement.Text(1),
        RequestId(statement.Text(2)),
        ContentDigest(statement.Text(3)),
        statement.Text(4),
        ReadProjectionColumns(statement, 5),
        ReadProjectionColumns(statement, 10),
        statement.Text(15),
        statement.IsNull(16)
            ? std::nullopt
            : std::optional<ContentDigest>(ContentDigest(statement.Text(16))),
        logical_scene_codec_internal::DecodeSnapshot(statement.Text(17)),
        DecodeUpdateStatus(statement.Text(18)),
        static_cast<std::size_t>(statement.Integer(19)),
    };
    const auto stored_request =
        logical_scene_codec_internal::DecodeRequest(intent.request_json);
    if (intent.request_owner_epoch != request_owner_epoch.value() ||
        intent.request_id.empty() ||
        !core::IsExactLowercaseSha256Digest(intent.request_digest.value()) ||
        intent.request_json.empty() ||
        intent.request_json.size() > kMaximumLogicalSceneTransactionBytes ||
        manyforge::content_identity::sha256_digest(
            logical_scene_codec_internal::EncodeRequestDigestInput(
                stored_request)) != intent.request_digest.value() ||
        stored_request.request_context.request_owner_epoch.value() !=
            intent.request_owner_epoch ||
        stored_request.request_context.request_id != intent.request_id ||
        stored_request.expected_projection_revision != intent.prior ||
        intent.resolved_operation_json.empty() ||
        intent.resolved_operation_json.size() >
            kMaximumLogicalSceneTransactionBytes ||
        (intent.resolution_evidence_digest.has_value() &&
         !core::IsExactLowercaseSha256Digest(
             intent.resolution_evidence_digest->value())) ||
        intent.required_projection_count > 256U ||
        intent.proposed.retained_bundle_id != intent.prior.retained_bundle_id ||
        intent.proposed.graph_revision != intent.prior.graph_revision ||
        intent.proposed.scene_revision.scene_id !=
            intent.prior.scene_revision.scene_id ||
        intent.proposed.scene_revision != intent.candidate_snapshot.revision ||
        intent.prior.scene_revision.scene_id !=
            bootstrap_snapshot.revision.scene_id ||
        intent.candidate_snapshot.transaction_sequence != expected_sequence ||
        intent.candidate_snapshot.cell_id != bootstrap_snapshot.cell_id) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "durable logical-scene transaction intent is invalid");
    }
    const auto projection_evidence = ReadProjectionEvidenceState(
        database, expected_sequence, intent.proposed);
    const auto attempts = projection_evidence.attempt_count;
    const auto results = projection_evidence.result_count;
    if (attempts > intent.required_projection_count || results > attempts) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene projection evidence cardinality is invalid");
    }
    const auto outcome = ReadOutcomeForSequence(database, expected_sequence);
    if (!outcome.has_value()) {
      state.unresolved_transaction_sequence = expected_sequence;
      if (attempts != 0U || results != 0U ||
          intent.required_projection_count != 0U) {
        state.projection_desynchronized = true;
      }
      state.unresolved_native_attempt = true;
    } else {
      if (outcome->transaction_sequence != expected_sequence ||
          outcome->request_digest != intent.request_digest ||
          outcome->attempted_projection_count != attempts ||
          (outcome->projection_status !=
               SceneProjectionCommitStatusDTO::kFailedProjectionUncertain &&
           results != attempts)) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "logical-scene outcome does not match its durable intent");
      }
      if (outcome->success && attempts != intent.required_projection_count) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "successful scene outcome lacks complete projection evidence");
      }
      if ((outcome->success && !projection_evidence.all_results_committed &&
           attempts != 0U) ||
          (outcome->projection_status ==
               SceneProjectionCommitStatusDTO::kRejectedRollbackProven &&
           !projection_evidence.all_results_rollback_proven) ||
          (outcome->projection_status ==
               SceneProjectionCommitStatusDTO::kFailedProjectionUncertain &&
           !projection_evidence.any_result_uncertain &&
           !projection_evidence.any_result_committed && results == attempts)) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "scene outcome contradicts its native projection evidence");
      }
      if (outcome->projection_status ==
          SceneProjectionCommitStatusDTO::kFailedProjectionUncertain) {
        state.latest_uncertain_transaction_sequence = expected_sequence;
        if (expected_sequence >
            history.recovered_through_transaction_sequence) {
          state.projection_desynchronized = true;
        }
      }
    }
    records.push_back(
        StoredSceneRecord{std::move(intent), attempts, results, outcome});
    ++expected_sequence;
    state.next_transaction_sequence = expected_sequence;
  }
  state.resident_transaction_count = records.size();
  if (records.size() > kMaximumLogicalSceneTransactions) {
    throw CanonicalEmptySceneOwnerLimitError(
        "logical-scene transaction ledger exceeds its fixed bound");
  }
  if (history.recovered_through_transaction_sequence >=
      state.next_transaction_sequence) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene recovery watermark exceeds durable transaction "
        "history");
  }

  std::vector<bool> transaction_event_seen(records.size(), false);
  std::vector<bool> named_apply_event_seen(
      durable_state.named_apply_stages.size(), false);
  std::vector<ProjectionRevisionDTO> published_projections{replay_projection};
  Statement events(
      database,
      "SELECT owner_event_sequence,event_kind,transaction_sequence,"
      "named_apply_lineage_generation FROM logical_scene_owner_events "
      "WHERE owner_event_sequence>? ORDER BY owner_event_sequence LIMIT 513");
  events.Bind(1, static_cast<std::int64_t>(
                     history.compacted_through_owner_event_sequence));
  std::uint64_t expected_event_sequence = state.next_owner_event_sequence;
  std::size_t resident_event_count = 0U;
  bool history_sealed = false;
  while (events.Step()) {
    if (history_sealed) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene owner history continues after an unresolved or "
          "desynchronized event");
    }
    if (++resident_event_count > kMaximumLogicalSceneOwnerEvents ||
        expected_event_sequence > kMaximumSafeIntegral ||
        events.Integer(0) !=
            static_cast<std::int64_t>(expected_event_sequence)) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene owner event sequence is invalid");
    }
    const auto kind = events.Text(1);
    if (kind == "scene_transaction_intent") {
      if (events.IsNull(2) || !events.IsNull(3)) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "scene transaction owner event has invalid referent presence");
      }
      const auto sequence = events.Integer(2);
      if (sequence <= 0 ||
          static_cast<std::uint64_t>(sequence) <=
              history.compacted_through_transaction_sequence ||
          static_cast<std::uint64_t>(sequence) >= expected_sequence) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "scene transaction owner event references an absent intent");
      }
      const auto index = static_cast<std::size_t>(
          static_cast<std::uint64_t>(sequence) -
          history.compacted_through_transaction_sequence - 1U);
      if (index >= records.size()) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "scene transaction owner event exceeds resident history");
      }
      auto &record = records[index];
      if (transaction_event_seen[index] ||
          record.intent.prior != state.active_projection) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "scene transaction event does not follow the active composite");
      }
      transaction_event_seen[index] = true;
      if (!record.outcome.has_value()) {
        history_sealed = true;
      } else if (record.outcome->success) {
        state.active_projection = record.intent.proposed;
        state.active_snapshot = record.intent.candidate_snapshot;
        published_projections.push_back(state.active_projection);
      } else if (record.outcome->projection_status ==
                     SceneProjectionCommitStatusDTO::
                         kFailedProjectionUncertain &&
                 record.intent.transaction_sequence >
                     history.recovered_through_transaction_sequence) {
        history_sealed = true;
      }
    } else if (kind == "named_apply_finalized") {
      if (!events.IsNull(2) || events.IsNull(3)) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "named Apply owner event has invalid referent presence");
      }
      const auto lineage = events.Integer(3);
      if (lineage <= 0 ||
          static_cast<std::size_t>(lineage) <=
              history.compacted_named_apply_stage_count ||
          static_cast<std::size_t>(lineage) >
              durable_state.named_apply_stages.size()) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "named Apply owner event references an absent stage");
      }
      const auto index = static_cast<std::size_t>(lineage - 1);
      const auto &stage = durable_state.named_apply_stages[index];
      if (named_apply_event_seen[index] ||
          !stage.finalization_receipt.has_value() ||
          stage.predecessor != state.active_projection) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "named Apply event does not follow the active composite");
      }
      named_apply_event_seen[index] = true;
      state.active_projection = stage.candidate;
      if (state.active_snapshot.revision !=
          state.active_projection.scene_revision) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "named Apply event changes logical-scene content");
      }
      published_projections.push_back(state.active_projection);
    } else {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene owner event kind is invalid");
    }
    ++expected_event_sequence;
  }
  state.next_owner_event_sequence = expected_event_sequence;

  for (std::size_t index = 0U; index < records.size(); ++index) {
    if (!transaction_event_seen[index]) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene intent lacks its owner-order event");
    }
  }
  for (std::size_t index = 0U; index < durable_state.named_apply_stages.size();
       ++index) {
    const auto &stage = durable_state.named_apply_stages[index];
    if (index < history.compacted_named_apply_stage_count) {
      if (!stage.discarded && !stage.finalization_receipt.has_value()) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "logical-scene checkpoint includes an open named Apply");
      }
      continue;
    }
    if (stage.finalization_receipt.has_value() !=
        named_apply_event_seen[index]) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "named Apply finalization and owner-order event disagree");
    }
    if (!stage.finalization_receipt.has_value()) {
      const auto predecessor_was_published =
          std::find(published_projections.begin(), published_projections.end(),
                    stage.predecessor) != published_projections.end();
      if (!predecessor_was_published ||
          (!stage.discarded && stage.predecessor != state.active_projection)) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "non-finalized named Apply did not capture an active composite");
      }
    }
  }
  if (!durable_state.active_projection.has_value() ||
      durable_state.active_projection->retained_bundle_id !=
          state.active_projection.retained_bundle_id ||
      durable_state.active_projection->graph_revision !=
          state.active_projection.graph_revision ||
      durable_state.active_projection->scene_revision.scene_id !=
          state.active_projection.scene_revision.scene_id ||
      durable_state.active_projection->scene_revision.revision_id >
          state.active_projection.scene_revision.revision_id ||
      (durable_state.active_projection->scene_revision.revision_id ==
           state.active_projection.scene_revision.revision_id &&
       durable_state.active_projection->scene_revision.content_hash !=
           state.active_projection.scene_revision.content_hash)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "typed structural head disagrees with owner-event replay");
  }
  return state;
}

ErrorDTO SceneError(const std::string_view code, const std::string_view message,
                    std::map<std::string, std::string> details = {}) {
  return {std::string(code), std::string(message), std::move(details)};
}

SceneCommitResultDTO
FailedSceneResult(const RequestId &request_id,
                  const core::RequestReplayStatusDTO replay, ErrorDTO error,
                  const SceneProjectionCommitStatusDTO projection_status) {
  SceneCommitResultDTO result{
      request_id,        replay,       false,
      std::move(error),  std::nullopt, std::nullopt,
      projection_status, std::nullopt,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot construct a valid failed scene result");
  }
  return result;
}

void ValidateBoundedText(const std::string_view value,
                         const std::string_view label,
                         const bool allow_empty = false) {
  if ((!allow_empty && value.empty()) ||
      value.size() > kMaximumSceneStoreIdentifierBytes ||
      !manyforge::content_identity::is_utf8_nfc(value)) {
    throw std::invalid_argument(std::string(label) +
                                " must be bounded NFC text");
  }
}

bool SamePose(const core::Pose3DTO &left,
              const core::Pose3DTO &right) noexcept {
  return left == right;
}

bool SameProviderEvidence(
    const std::optional<core::SceneObjectProviderEvidenceDTO> &left,
    const std::optional<core::SceneObjectProviderEvidenceDTO> &right) {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  if (!left.has_value()) {
    return true;
  }
  return left->provider_instance_id == right->provider_instance_id &&
         left->provider_manifest_id == right->provider_manifest_id &&
         left->provider_manifest_revision ==
             right->provider_manifest_revision &&
         left->provider_source_epoch == right->provider_source_epoch &&
         left->provider_source_sequence == right->provider_source_sequence &&
         left->received_at_ns == right->received_at_ns &&
         left->source_timestamp_ns == right->source_timestamp_ns &&
         left->source_freshness_budget_ns == right->source_freshness_budget_ns;
}

template <typename Value, typename Projection>
bool CanonicalVector(const std::vector<Value> &values, Projection projection) {
  for (std::size_t index = 1U; index < values.size(); ++index) {
    if (!(projection(values[index - 1U]) < projection(values[index]))) {
      return false;
    }
  }
  return true;
}

struct ResolvedSceneCandidate final {
  LogicalSceneSnapshotDTO snapshot;
  core::ResolvedSceneOperationDTO operation;
  SceneUpdateStatusDTO update_status;
  std::optional<ContentDigest> resolution_evidence_digest;
};

ContentDigest CellResolutionEvidence(const ContentDigest &request_digest,
                                     const ProjectionRevisionDTO &projection) {
  return ContentDigest(manyforge::content_identity::sha256_digest(
      "manyforge.scene-frame-resolution.cell.v1\n" + request_digest.value() +
      "\n" + projection.retained_bundle_id.value() + "\n" +
      projection.graph_revision.value() + "\n" +
      projection.scene_revision.content_hash.value()));
}

class SceneRequestRejectionError final : public std::runtime_error {
public:
  SceneRequestRejectionError(std::string code, std::string message,
                             std::map<std::string, std::string> details = {})
      : std::runtime_error(std::move(message)), code_(std::move(code)),
        details_(std::move(details)) {}

  [[nodiscard]] const std::string &code() const noexcept { return code_; }
  [[nodiscard]] const std::map<std::string, std::string> &
  details() const noexcept {
    return details_;
  }

private:
  std::string code_;
  std::map<std::string, std::string> details_;
};

bool ReleaseFieldsMatchDefaults(const core::SupportContactDTO &contact) {
  return contact.enforce_max_start_penetration &&
         contact.max_start_penetration_m == 0.0 &&
         contact.release_tolerance_m == 1e-5 &&
         contact.min_release_progress_m == 0.0 &&
         contact.require_separation_by_end &&
         contact.require_strictly_decreasing_penetration &&
         !contact.allow_recontact;
}

bool ReleaseFieldsAreNeutral(const core::SupportContactDTO &contact) {
  return !contact.enforce_max_start_penetration &&
         contact.max_start_penetration_m == 0.0 &&
         contact.release_tolerance_m == 0.0 &&
         contact.min_release_progress_m == 0.0 &&
         !contact.require_separation_by_end &&
         !contact.require_strictly_decreasing_penetration &&
         !contact.allow_recontact;
}

bool BinaryFieldsMatchDefaults(const core::SupportContactDTO &contact) {
  return contact.enforce_max_start_penetration &&
         contact.max_start_penetration_m == 0.0 &&
         contact.release_tolerance_m == 1e-5 &&
         contact.min_release_progress_m == 0.0 &&
         contact.require_strictly_decreasing_penetration;
}

bool BinaryFieldsAreNeutral(const core::SupportContactDTO &contact) {
  return !contact.enforce_max_start_penetration &&
         contact.max_start_penetration_m == 0.0 &&
         contact.release_tolerance_m == 0.0 &&
         contact.min_release_progress_m == 0.0 &&
         !contact.require_strictly_decreasing_penetration;
}

[[noreturn]] void RejectSupportContact(const std::size_t index,
                                       const std::string_view reason,
                                       const std::string_view field,
                                       const std::string_view message) {
  throw SceneRequestRejectionError(
      "invalid_contact_policy", std::string(message),
      {{"support_contact_index", std::to_string(index)},
       {"reason", std::string(reason)},
       {"field", std::string(field)}});
}

void ValidateAndNormalizeSupportContact(core::SupportContactDTO &contact,
                                        const std::size_t index) {
  try {
    ValidateBoundedIdentity(contact.support_object_id, "support_object_id");
  } catch (const std::invalid_argument &error) {
    RejectSupportContact(index, "invalid_support_contact_reference",
                         "support_object_id", error.what());
  }
  const auto nonnegative_finite = [](const double value) {
    return std::isfinite(value) && value >= 0.0;
  };
  if (!nonnegative_finite(contact.max_start_penetration_m) ||
      !nonnegative_finite(contact.release_tolerance_m) ||
      !nonnegative_finite(contact.min_release_progress_m) ||
      !nonnegative_finite(contact.max_contact_penetration_m)) {
    RejectSupportContact(index, "support_contact_metric_invalid", "metrics",
                         "support-contact metrics must be finite and "
                         "non-negative");
  }

  switch (contact.mode) {
  case core::CollisionContactPolicyMode::kBoundedPenetration:
    if (contact.release_evidence_mode !=
        core::ContactReleaseEvidenceMode::kSignedPenetration) {
      RejectSupportContact(index, "release_evidence_mode_not_applicable",
                           "release_evidence_mode",
                           "binary release evidence is not applicable to "
                           "bounded penetration");
    }
    if (!ReleaseFieldsMatchDefaults(contact) &&
        !ReleaseFieldsAreNeutral(contact)) {
      RejectSupportContact(index, "release_field_for_bounded_penetration",
                           "release_fields",
                           "release-only fields are not applicable to "
                           "bounded penetration");
    }
    break;
  case core::CollisionContactPolicyMode::kContactProcess:
    if (contact.max_contact_penetration_m != 0.0) {
      RejectSupportContact(index, "bounded_penetration_field_not_applicable",
                           "max_contact_penetration_m",
                           "max_contact_penetration_m is valid only for "
                           "bounded penetration");
    }
    if (contact.release_evidence_mode !=
            core::ContactReleaseEvidenceMode::kSignedPenetration ||
        (!ReleaseFieldsMatchDefaults(contact) &&
         !ReleaseFieldsAreNeutral(contact))) {
      RejectSupportContact(index, "release_field_not_applicable",
                           "release_fields",
                           "release-only fields are not applicable to "
                           "contact process");
    }
    break;
  case core::CollisionContactPolicyMode::kContactRelease:
    if (contact.max_contact_penetration_m != 0.0) {
      RejectSupportContact(index, "bounded_penetration_field_not_applicable",
                           "max_contact_penetration_m",
                           "max_contact_penetration_m is valid only for "
                           "bounded penetration");
    }
    switch (contact.release_evidence_mode) {
    case core::ContactReleaseEvidenceMode::kSignedPenetration:
      break;
    case core::ContactReleaseEvidenceMode::kBinaryContact:
      if (!BinaryFieldsMatchDefaults(contact) &&
          !BinaryFieldsAreNeutral(contact)) {
        RejectSupportContact(index, "metric_field_for_binary_contact",
                             "metric_fields",
                             "signed-distance metric fields are not valid "
                             "for binary contact evidence");
      }
      break;
    default:
      RejectSupportContact(index, "unknown_release_evidence_mode",
                           "release_evidence_mode",
                           "support-contact release evidence is unsupported");
    }
    break;
  default:
    RejectSupportContact(index, "unknown_contact_policy_mode", "mode",
                         "support-contact mode is unsupported");
  }

  if (contact.mode == core::CollisionContactPolicyMode::kContactRelease) {
    contact.max_contact_penetration_m = 0.0;
    if (contact.release_evidence_mode ==
        core::ContactReleaseEvidenceMode::kBinaryContact) {
      contact.enforce_max_start_penetration = false;
      contact.max_start_penetration_m = 0.0;
      contact.release_tolerance_m = 0.0;
      contact.min_release_progress_m = 0.0;
      contact.require_strictly_decreasing_penetration = false;
    }
    return;
  }

  contact.release_evidence_mode =
      core::ContactReleaseEvidenceMode::kSignedPenetration;
  contact.enforce_max_start_penetration = false;
  contact.max_start_penetration_m = 0.0;
  contact.release_tolerance_m = 0.0;
  contact.min_release_progress_m = 0.0;
  contact.require_separation_by_end = false;
  contact.require_strictly_decreasing_penetration = false;
  contact.allow_recontact = false;
  if (contact.mode == core::CollisionContactPolicyMode::kContactProcess) {
    contact.max_contact_penetration_m = 0.0;
  }
}

std::vector<core::SupportContactDTO>
NormalizeSupportContacts(const core::SceneResourceId &scene_resource_id,
                         std::vector<core::SupportContactDTO> support_contacts,
                         const core::LogicalSceneSnapshotDTO &current) {
  for (std::size_t index = 0U; index < support_contacts.size(); ++index) {
    ValidateAndNormalizeSupportContact(support_contacts[index], index);
  }
  std::sort(support_contacts.begin(), support_contacts.end(),
            [](const auto &left, const auto &right) {
              return left.support_object_id < right.support_object_id;
            });
  for (std::size_t index = 0U; index < support_contacts.size(); ++index) {
    const auto &contact = support_contacts[index];
    if (index != 0U && support_contacts[index - 1U].support_object_id ==
                           contact.support_object_id) {
      RejectSupportContact(index, "duplicate_support_contact_pair",
                           "support_object_id",
                           "support object is declared more than once");
    }
    if (contact.support_object_id == scene_resource_id.value()) {
      RejectSupportContact(index, "self_support_contact", "support_object_id",
                           "scene resource cannot support itself");
    }
    const auto support =
        std::lower_bound(current.objects.begin(), current.objects.end(),
                         contact.support_object_id,
                         [](const SceneObjectSnapshotDTO &object,
                            const std::string &support_id) {
                           return object.scene_resource_id.value() < support_id;
                         });
    if (support == current.objects.end() ||
        support->scene_resource_id.value() != contact.support_object_id) {
      throw SceneRequestRejectionError(
          "missing_reference",
          "support contact names an unknown scene resource",
          {{"reference_type", "support_scene_resource"},
           {"reference_id", contact.support_object_id},
           {"scene_resource_id", scene_resource_id.value()}});
    }
  }
  return support_contacts;
}

const core::CellSceneFrameRefDTO &
RequireCellFrame(const core::SceneTransactionFrameRefDTO &frame,
                 const CellId &cell_id) {
  const auto *cell = std::get_if<core::CellSceneFrameRefDTO>(&frame);
  if (cell == nullptr) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cell-frame normalizer received another frame-reference kind");
  }
  if (cell->cell_id != cell_id) {
    throw SceneRequestRejectionError(
        "missing_reference",
        "scene frame does not name the active semantic cell",
        {{"reference_type", "cell_frame"},
         {"reference_id", cell->cell_id.value()}});
  }
  return *cell;
}

[[nodiscard]] bool
FrameReferenceMatchesResolved(const core::SceneTransactionFrameRefDTO &reference,
                              const core::ResolvedFrameDTO &resolved) noexcept {
  return std::visit(
      [&](const auto &selected) {
        using T = std::remove_cvref_t<decltype(selected)>;
        if constexpr (std::is_same_v<T, core::CellSceneFrameRefDTO>) {
          const auto *frame =
              std::get_if<core::ResolvedCellFrameDTO>(&resolved);
          return frame != nullptr && frame->cell_id == selected.cell_id;
        } else if constexpr (std::is_same_v<T,
                                            core::ComponentSceneFrameRefDTO>) {
          const auto *frame =
              std::get_if<core::ResolvedComponentModelFrameDTO>(&resolved);
          return frame != nullptr &&
                 frame->controllable_component_id ==
                     selected.controllable_component_id &&
                 frame->model_instance_id == selected.model_instance_id &&
                 frame->frame_role_id == selected.frame_role_id;
        } else if constexpr (std::is_same_v<
                                 T, core::PassiveModelSceneFrameRefDTO>) {
          const auto *frame =
              std::get_if<core::ResolvedPassiveModelFrameDTO>(&resolved);
          return frame != nullptr &&
                 frame->model_instance_id == selected.model_instance_id &&
                 frame->frame_role_id == selected.frame_role_id;
        } else {
          return false;
        }
      },
      reference);
}

struct NormalizedScenePose final {
  core::Pose3DTO pose_in_cell;
  ContentDigest resolution_evidence_digest;
};

[[nodiscard]] NormalizedScenePose
NormalizeScenePose(const core::SceneTransactionFrameRefDTO &frame_ref,
                   const core::Pose3DTO &pose_in_frame,
                   const LogicalSceneSnapshotDTO &current,
                   const ProjectionRevisionDTO &expected_projection,
                   const ContentDigest &request_digest,
                   const std::optional<core::SceneFrameResolutionReceiptDTO>
                       &frame_resolution) {
  if (std::holds_alternative<core::CellSceneFrameRefDTO>(frame_ref)) {
    static_cast<void>(RequireCellFrame(frame_ref, current.cell_id));
    if (frame_resolution.has_value()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "cell-frame normalization unexpectedly carries native evidence");
    }
    return {pose_in_frame,
            CellResolutionEvidence(request_digest, expected_projection)};
  }
  if (const auto *scene_resource =
          std::get_if<core::SceneResourceSceneFrameRefDTO>(&frame_ref)) {
    ValidateBoundedIdentity(scene_resource->scene_resource_id.value(),
                            "scene_resource_id");
    const auto reference =
        std::lower_bound(current.objects.begin(), current.objects.end(),
                         scene_resource->scene_resource_id,
                         [](const SceneObjectSnapshotDTO &object,
                            const SceneResourceId &id) {
                           return object.scene_resource_id < id;
                         });
    if (reference == current.objects.end() ||
        reference->scene_resource_id != scene_resource->scene_resource_id) {
      throw SceneRequestRejectionError(
          "missing_reference", "scene-resource frame does not exist",
          {{"reference_type", "scene_resource"},
           {"reference_id", scene_resource->scene_resource_id.value()}});
    }
    const auto *fixed =
        std::get_if<core::CellFixedSceneObjectPlacementDTO>(
            &reference->placement);
    if (fixed != nullptr) {
      if (frame_resolution.has_value()) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "fixed scene-resource normalization unexpectedly carries native "
            "evidence");
      }
      return {core::multiply_poses(fixed->pose_in_cell, pose_in_frame),
              CellResolutionEvidence(request_digest, expected_projection)};
    }
    const auto &carried =
        std::get<core::CarriedSceneObjectPlacementDTO>(reference->placement);
    const auto attachment = std::lower_bound(
        current.carried_object_attachments.begin(),
        current.carried_object_attachments.end(),
        carried.carried_object_attachment_id,
        [](const core::CarriedObjectAttachmentDTO &entry,
           const core::CarriedObjectAttachmentId &id) {
          return entry.carried_object_attachment_id < id;
        });
    if (attachment == current.carried_object_attachments.end() ||
        attachment->carried_object_attachment_id !=
            carried.carried_object_attachment_id ||
        attachment->carried_object_attachment_revision !=
            carried.carried_object_attachment_revision) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "carried scene-resource frame has no exact attachment row");
    }
    if (!frame_resolution.has_value() || !frame_resolution->valid() ||
        frame_resolution->projection_binding.projection_revision !=
            expected_projection ||
        !FrameReferenceMatchesResolved(
            core::ToSceneTransactionFrameRef(
                attachment->carrier_frame_ref),
            frame_resolution->resolved_frame)) {
      throw SceneRequestRejectionError(
          "missing_reference",
          "carried scene-resource frame has no exact immutable retained "
          "resolution",
          {{"reason", "carried_scene_resource_resolution_missing_or_foreign"},
           {"reference_id", scene_resource->scene_resource_id.value()}});
    }
    const auto resource_pose_in_cell = core::multiply_poses(
        frame_resolution->frame_pose_in_cell,
        attachment->pose_in_carrier_frame);
    return {core::multiply_poses(resource_pose_in_cell, pose_in_frame),
            frame_resolution->resolution_evidence_digest};
  }
  if (!frame_resolution.has_value() || !frame_resolution->valid() ||
      frame_resolution->projection_binding.projection_revision !=
          expected_projection ||
      !FrameReferenceMatchesResolved(frame_ref,
                                     frame_resolution->resolved_frame)) {
    throw SceneRequestRejectionError(
        "missing_reference",
        "scene frame has no exact immutable retained resolution",
        {{"reason", "scene_frame_resolution_missing_or_foreign"}});
  }
  return {
      core::multiply_poses(frame_resolution->frame_pose_in_cell, pose_in_frame),
      frame_resolution->resolution_evidence_digest};
}

std::string CurrentFrameId(const LogicalSceneSnapshotDTO &snapshot,
                           const SceneObjectSnapshotDTO &object) {
  if (const auto *cell = std::get_if<core::CellFixedSceneObjectPlacementDTO>(
          &object.placement)) {
    return cell->cell_id.value();
  }
  const auto &carried =
      std::get<core::CarriedSceneObjectPlacementDTO>(object.placement);
  const auto attachment =
      std::lower_bound(snapshot.carried_object_attachments.begin(),
                       snapshot.carried_object_attachments.end(),
                       carried.carried_object_attachment_id,
                       [](const core::CarriedObjectAttachmentDTO &entry,
                          const core::CarriedObjectAttachmentId &id) {
                         return entry.carried_object_attachment_id < id;
                       });
  if (attachment == snapshot.carried_object_attachments.end() ||
      attachment->carried_object_attachment_id !=
          carried.carried_object_attachment_id ||
      attachment->carried_object_attachment_revision !=
          carried.carried_object_attachment_revision) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "carried scene object has no exact attachment row");
  }
  const auto *frame = std::get_if<core::ComponentSceneFrameRefDTO>(
      &attachment->carrier_frame_ref);
  if (frame == nullptr) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "carried scene object attachment has no component frame");
  }
  return frame->frame_role_id.value();
}

ResolvedSceneCandidate
ResolveSceneCandidate(const SceneTransactionRequestDTO &request,
                      const LogicalSceneSnapshotDTO &current,
                      const std::uint64_t transaction_sequence,
                      const ContentDigest &request_digest,
                      const std::optional<core::SceneFrameResolutionReceiptDTO>
                          &frame_resolution) {
  ResolvedSceneCandidate candidate{
      current,
      core::ResolvedRemoveSceneObjectDTO{SceneResourceId(std::string{})},
      SceneUpdateStatusDTO::kGeometryChanged,
      std::nullopt,
  };
  candidate.snapshot.transaction_sequence = transaction_sequence;
  bool content_expected_to_change = true;
  bool topology_changed = true;
  bool policy_changed = false;

  std::visit(
      [&](const auto &operation) {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, core::AddObjectRequestDTO> ||
                      std::is_same_v<T, core::UpsertObjectRequestDTO>) {
          ValidateBoundedIdentity(operation.scene_resource_id.value(),
                                  "scene_resource_id");
          const auto normalized =
              NormalizeScenePose(operation.frame_ref, operation.pose, current,
                                 request.expected_projection_revision,
                                 request_digest, frame_resolution);
          const auto found = std::lower_bound(
              candidate.snapshot.objects.begin(),
              candidate.snapshot.objects.end(), operation.scene_resource_id,
              [](const SceneObjectSnapshotDTO &object,
                 const SceneResourceId &id) {
                return object.scene_resource_id < id;
              });
          const bool exists =
              found != candidate.snapshot.objects.end() &&
              found->scene_resource_id == operation.scene_resource_id;
          if constexpr (std::is_same_v<T, core::AddObjectRequestDTO>) {
            if (exists) {
              throw SceneRequestRejectionError(
                  "object_already_exists", "scene resource already exists",
                  {{"scene_resource_id", operation.scene_resource_id.value()}});
            }
          } else if (exists && std::holds_alternative<
                                   core::CarriedSceneObjectPlacementDTO>(
                                   found->placement)) {
            throw SceneRequestRejectionError(
                "scene_object_state_conflict",
                "a carried object cannot be replaced",
                {{"reason", "scene_object_is_attached"},
                 {"scene_resource_id", operation.scene_resource_id.value()}});
          }
          auto support_contacts = NormalizeSupportContacts(
              operation.scene_resource_id, operation.support_contacts, current);
          SceneObjectSnapshotDTO object{
              operation.scene_resource_id,
              {operation.shape},
              core::CellFixedSceneObjectPlacementDTO{current.cell_id,
                                                     normalized.pose_in_cell},
              std::nullopt,
              std::move(support_contacts),
          };
          const bool unchanged = exists && *found == object;
          if (exists) {
            *found = object;
          } else {
            candidate.snapshot.objects.insert(found, object);
          }
          candidate.operation = core::ResolvedUpsertSceneObjectDTO{object};
          candidate.resolution_evidence_digest =
              normalized.resolution_evidence_digest;
          if (unchanged) {
            candidate.update_status = SceneUpdateStatusDTO::kNoOp;
            content_expected_to_change = false;
            topology_changed = false;
          }
        } else if constexpr (std::is_same_v<T, core::RemoveObjectRequestDTO>) {
          ValidateBoundedIdentity(operation.scene_resource_id.value(),
                                  "scene_resource_id");
          const auto found = std::lower_bound(
              candidate.snapshot.objects.begin(),
              candidate.snapshot.objects.end(), operation.scene_resource_id,
              [](const SceneObjectSnapshotDTO &object,
                 const SceneResourceId &id) {
                return object.scene_resource_id < id;
              });
          if (found == candidate.snapshot.objects.end() ||
              found->scene_resource_id != operation.scene_resource_id) {
            throw SceneRequestRejectionError(
                "object_not_found", "scene resource does not exist",
                {{"scene_resource_id", operation.scene_resource_id.value()}});
          }
          for (const auto &object : candidate.snapshot.objects) {
            if (object.scene_resource_id == operation.scene_resource_id) {
              continue;
            }
            const auto dependent = std::find_if(
                object.support_contacts.begin(), object.support_contacts.end(),
                [&](const core::SupportContactDTO &contact) {
                  return contact.support_object_id ==
                         operation.scene_resource_id.value();
                });
            if (dependent != object.support_contacts.end()) {
              throw SceneRequestRejectionError(
                  "scene_object_state_conflict",
                  "scene resource is still referenced as a support",
                  {{"reason", "support_contact_reference_exists"},
                   {"scene_resource_id", operation.scene_resource_id.value()},
                   {"dependent_scene_resource_id",
                    object.scene_resource_id.value()}});
            }
          }
          if (std::holds_alternative<core::CarriedSceneObjectPlacementDTO>(
                  found->placement)) {
            throw CanonicalEmptySceneOwnerConflictError(
                "a carried object must be detached before removal");
          }
          candidate.snapshot.objects.erase(found);
          candidate.operation =
              core::ResolvedRemoveSceneObjectDTO{operation.scene_resource_id};
        } else if constexpr (std::is_same_v<T,
                                            core::UpdateObjectPoseRequestDTO>) {
          ValidateBoundedIdentity(operation.scene_resource_id.value(),
                                  "scene_resource_id");
          const auto normalized =
              NormalizeScenePose(operation.frame_ref, operation.pose, current,
                                 request.expected_projection_revision,
                                 request_digest, frame_resolution);
          auto found = std::lower_bound(candidate.snapshot.objects.begin(),
                                        candidate.snapshot.objects.end(),
                                        operation.scene_resource_id,
                                        [](const SceneObjectSnapshotDTO &object,
                                           const SceneResourceId &id) {
                                          return object.scene_resource_id < id;
                                        });
          if (found == candidate.snapshot.objects.end() ||
              found->scene_resource_id != operation.scene_resource_id) {
            throw SceneRequestRejectionError(
                "object_not_found", "scene resource does not exist",
                {{"scene_resource_id", operation.scene_resource_id.value()}});
          }
          const bool attached =
              std::holds_alternative<core::CarriedSceneObjectPlacementDTO>(
                  found->placement);
          if (operation.expected_attached.has_value() &&
              *operation.expected_attached != attached) {
            throw SceneRequestRejectionError(
                "scene_object_state_conflict",
                "scene object attachment precondition failed",
                {{"reason", "scene_object_attachment_precondition_failed"},
                 {"scene_resource_id", operation.scene_resource_id.value()},
                 {"expected_attached",
                  *operation.expected_attached ? "true" : "false"},
                 {"current_attached", attached ? "true" : "false"},
                 {"current_frame_id", CurrentFrameId(current, *found)}});
          }
          if (attached) {
            throw SceneRequestRejectionError(
                "scene_object_state_conflict",
                "a carried object pose is derived from its attachment",
                {{"reason", "scene_object_is_attached"},
                 {"scene_resource_id", operation.scene_resource_id.value()},
                 {"current_frame_id", CurrentFrameId(current, *found)}});
          }
          if (operation.provider_evidence.has_value() &&
              found->provider_evidence.has_value()) {
            const auto &next = *operation.provider_evidence;
            const auto &previous = *found->provider_evidence;
            if (next.provider_instance_id == previous.provider_instance_id &&
                next.provider_manifest_id == previous.provider_manifest_id &&
                next.provider_manifest_revision ==
                    previous.provider_manifest_revision &&
                next.provider_source_epoch == previous.provider_source_epoch &&
                next.provider_source_sequence <=
                    previous.provider_source_sequence) {
              throw SceneRequestRejectionError(
                  "scene_object_state_conflict",
                  "provider source sequence did not advance",
                  {{"reason", "provider_source_sequence_not_increasing"},
                   {"scene_resource_id", operation.scene_resource_id.value()},
                   {"provider_instance_id", next.provider_instance_id.value()},
                   {"provider_source_epoch", next.provider_source_epoch},
                   {"previous_provider_source_sequence",
                    std::to_string(previous.provider_source_sequence)},
                   {"received_provider_source_sequence",
                    std::to_string(next.provider_source_sequence)}});
            }
          }
          const auto previous_pose =
              std::get<core::CellFixedSceneObjectPlacementDTO>(found->placement)
                  .pose_in_cell;
          const bool pose_changed =
              !SamePose(previous_pose, normalized.pose_in_cell);
          const bool evidence_changed = !SameProviderEvidence(
              found->provider_evidence, operation.provider_evidence);
          found->placement = core::CellFixedSceneObjectPlacementDTO{
              current.cell_id, normalized.pose_in_cell};
          found->provider_evidence = operation.provider_evidence;
          candidate.operation = core::ResolvedUpsertSceneObjectDTO{*found};
          candidate.resolution_evidence_digest =
              normalized.resolution_evidence_digest;
          if (!pose_changed && !evidence_changed) {
            candidate.update_status = SceneUpdateStatusDTO::kNoOp;
            content_expected_to_change = false;
            topology_changed = false;
          } else if (!pose_changed) {
            candidate.update_status = SceneUpdateStatusDTO::kFreshnessOnly;
            topology_changed = false;
          }
        } else if constexpr (std::is_same_v<
                                 T, core::AttachCarriedObjectRequestDTO>) {
          ValidateBoundedIdentity(
              operation.carried_object_attachment_id.value(),
              "carried_object_attachment_id");
          ValidateBoundedIdentity(operation.scene_resource_id.value(),
                                  "scene_resource_id");
          if (!frame_resolution.has_value() || !frame_resolution->valid() ||
              frame_resolution->projection_binding.projection_revision !=
                  request.expected_projection_revision) {
            throw SceneRequestRejectionError(
                "missing_reference",
                "carried-object attach has no exact carrier-frame resolution",
                {{"reason", "carrier_frame_resolution_missing"}});
          }
          const auto *carrier =
              std::get_if<core::ResolvedComponentModelFrameDTO>(
                  &frame_resolution->resolved_frame);
          if (carrier == nullptr ||
              carrier->controllable_component_id !=
                  operation.controllable_component_id) {
            throw SceneRequestRejectionError(
                "missing_reference",
                "carrier target resolved to another component",
                {{"reason", "carrier_frame_resolution_foreign"}});
          }
          auto expected_revisions =
              operation.expected_component_attachment_revisions;
          if (!std::is_sorted(expected_revisions.begin(),
                              expected_revisions.end()) ||
              std::adjacent_find(expected_revisions.begin(),
                                 expected_revisions.end()) !=
                  expected_revisions.end() ||
              expected_revisions !=
                  frame_resolution->component_attachment_revisions) {
            throw SceneRequestRejectionError(
                "stale_revision",
                "carrier structural-attachment precondition failed",
                {{"reason", "carrier_attachment_revision_mismatch"}});
          }
          auto touch_roles = operation.touch_body_roles;
          if (std::any_of(touch_roles.begin(), touch_roles.end(),
                          [](const auto &role) { return role.empty(); }) ||
              !std::is_sorted(touch_roles.begin(), touch_roles.end()) ||
              std::adjacent_find(touch_roles.begin(), touch_roles.end()) !=
                  touch_roles.end()) {
            throw std::invalid_argument(
                "touch_body_roles must be sorted, unique, and non-empty "
                "when present");
          }
          const auto duplicate_attachment = std::lower_bound(
              candidate.snapshot.carried_object_attachments.begin(),
              candidate.snapshot.carried_object_attachments.end(),
              operation.carried_object_attachment_id,
              [](const core::CarriedObjectAttachmentDTO &entry,
                 const core::CarriedObjectAttachmentId &id) {
                return entry.carried_object_attachment_id < id;
              });
          if (duplicate_attachment !=
                  candidate.snapshot.carried_object_attachments.end() &&
              duplicate_attachment->carried_object_attachment_id ==
                  operation.carried_object_attachment_id) {
            throw SceneRequestRejectionError(
                "scene_object_state_conflict",
                "carried-object attachment ID already exists",
                {{"reason", "carried_attachment_already_exists"}});
          }
          auto object = std::lower_bound(candidate.snapshot.objects.begin(),
                                         candidate.snapshot.objects.end(),
                                         operation.scene_resource_id,
                                         [](const SceneObjectSnapshotDTO &entry,
                                            const SceneResourceId &id) {
                                           return entry.scene_resource_id < id;
                                         });
          if (object == candidate.snapshot.objects.end() ||
              object->scene_resource_id != operation.scene_resource_id) {
            throw SceneRequestRejectionError(
                "object_not_found", "scene resource does not exist",
                {{"scene_resource_id", operation.scene_resource_id.value()}});
          }
          const auto *fixed =
              std::get_if<core::CellFixedSceneObjectPlacementDTO>(
                  &object->placement);
          if (fixed == nullptr) {
            throw SceneRequestRejectionError(
                "scene_object_state_conflict",
                "scene resource is already carried",
                {{"reason", "scene_object_is_attached"},
                 {"scene_resource_id", operation.scene_resource_id.value()}});
          }
          core::CarriedObjectAttachmentDTO attachment{
              operation.carried_object_attachment_id,
              core::CarriedObjectAttachmentRevision(std::string{}),
              operation.scene_resource_id,
              operation.controllable_component_id,
              operation.command_target_id,
              core::ComponentSceneFrameRefDTO{
                  carrier->controllable_component_id,
                  carrier->model_instance_id, carrier->frame_role_id},
              core::multiply_poses(
                  core::inverse_pose(frame_resolution->frame_pose_in_cell),
                  fixed->pose_in_cell),
              std::move(touch_roles)};
          attachment.carried_object_attachment_revision =
              core::logical_scene_identity::
                  ComputeCarriedObjectAttachmentRevision(attachment);
          object->placement = core::CarriedSceneObjectPlacementDTO{
              attachment.carried_object_attachment_id,
              attachment.carried_object_attachment_revision};
          object->provider_evidence = std::nullopt;
          candidate.snapshot.carried_object_attachments.insert(
              duplicate_attachment, attachment);
          candidate.operation = core::ResolvedAttachCarriedObjectDTO{
              *object, std::move(attachment)};
          candidate.resolution_evidence_digest =
              frame_resolution->resolution_evidence_digest;
          policy_changed = true;
        } else if constexpr (std::is_same_v<
                                 T, core::DetachCarriedObjectRequestDTO>) {
          const auto normalized =
              NormalizeScenePose(operation.frame_ref, operation.pose, current,
                                 request.expected_projection_revision,
                                 request_digest, frame_resolution);
          const auto attachment = std::lower_bound(
              candidate.snapshot.carried_object_attachments.begin(),
              candidate.snapshot.carried_object_attachments.end(),
              operation.carried_object_attachment_id,
              [](const core::CarriedObjectAttachmentDTO &entry,
                 const core::CarriedObjectAttachmentId &id) {
                return entry.carried_object_attachment_id < id;
              });
          if (attachment ==
                  candidate.snapshot.carried_object_attachments.end() ||
              attachment->carried_object_attachment_id !=
                  operation.carried_object_attachment_id ||
              attachment->carried_object_attachment_revision !=
                  operation.expected_carried_object_attachment_revision) {
            throw CanonicalEmptySceneOwnerConflictError(
                "carried-object attachment precondition failed");
          }
          const auto object = std::lower_bound(
              candidate.snapshot.objects.begin(),
              candidate.snapshot.objects.end(), attachment->scene_resource_id,
              [](const SceneObjectSnapshotDTO &entry,
                 const SceneResourceId &id) {
                return entry.scene_resource_id < id;
              });
          if (object == candidate.snapshot.objects.end() ||
              object->scene_resource_id != attachment->scene_resource_id) {
            throw CanonicalEmptySceneOwnerIntegrityError(
                "carried attachment has no matching scene object");
          }
          const auto removed_id = attachment->carried_object_attachment_id;
          const auto removed_revision =
              attachment->carried_object_attachment_revision;
          object->placement = core::CellFixedSceneObjectPlacementDTO{
              current.cell_id, normalized.pose_in_cell};
          candidate.snapshot.carried_object_attachments.erase(attachment);
          candidate.operation = core::ResolvedDetachCarriedObjectDTO{
              *object, removed_id, removed_revision};
          candidate.resolution_evidence_digest =
              normalized.resolution_evidence_digest;
          policy_changed = true;
        } else if constexpr (
            std::is_same_v<T,
                           core::DetachCarriedObjectAtCurrentPoseRequestDTO>) {
          ValidateBoundedIdentity(
              operation.carried_object_attachment_id.value(),
              "carried_object_attachment_id");
          ValidateBoundedIdentity(operation.expected_scene_resource_id.value(),
                                  "expected_scene_resource_id");
          ValidateBoundedIdentity(
              operation.expected_controllable_component_id.value(),
              "expected_controllable_component_id");
          ValidateBoundedIdentity(operation.expected_command_target_id.value(),
                                  "expected_command_target_id");
          const auto attachment = std::lower_bound(
              candidate.snapshot.carried_object_attachments.begin(),
              candidate.snapshot.carried_object_attachments.end(),
              operation.carried_object_attachment_id,
              [](const core::CarriedObjectAttachmentDTO &entry,
                 const core::CarriedObjectAttachmentId &id) {
                return entry.carried_object_attachment_id < id;
              });
          if (attachment ==
                  candidate.snapshot.carried_object_attachments.end() ||
              attachment->carried_object_attachment_id !=
                  operation.carried_object_attachment_id ||
              attachment->carried_object_attachment_revision !=
                  operation.expected_carried_object_attachment_revision ||
              attachment->scene_resource_id !=
                  operation.expected_scene_resource_id ||
              attachment->controllable_component_id !=
                  operation.expected_controllable_component_id ||
              attachment->command_target_id !=
                  operation.expected_command_target_id) {
            throw CanonicalEmptySceneOwnerConflictError(
                "carried-object attachment precondition failed");
          }
          if (!frame_resolution.has_value() || !frame_resolution->valid() ||
              frame_resolution->projection_binding.projection_revision !=
                  request.expected_projection_revision) {
            throw SceneRequestRejectionError(
                "missing_reference",
                "current carried-object pose has no exact carrier-frame "
                "resolution",
                {{"reason", "carrier_frame_resolution_missing"}});
          }
          const auto *expected_frame =
              std::get_if<core::ComponentSceneFrameRefDTO>(
                  &attachment->carrier_frame_ref);
          const auto *resolved_frame =
              std::get_if<core::ResolvedComponentModelFrameDTO>(
                  &frame_resolution->resolved_frame);
          if (expected_frame == nullptr || resolved_frame == nullptr ||
              resolved_frame->controllable_component_id !=
                  expected_frame->controllable_component_id ||
              resolved_frame->model_instance_id !=
                  expected_frame->model_instance_id ||
              resolved_frame->frame_role_id != expected_frame->frame_role_id) {
            throw SceneRequestRejectionError(
                "missing_reference",
                "current carrier frame resolved to another component, model, "
                "or frame role",
                {{"reason", "carrier_frame_resolution_foreign"}});
          }
          auto object = std::lower_bound(candidate.snapshot.objects.begin(),
                                         candidate.snapshot.objects.end(),
                                         attachment->scene_resource_id,
                                         [](const SceneObjectSnapshotDTO &entry,
                                            const SceneResourceId &id) {
                                           return entry.scene_resource_id < id;
                                         });
          if (object == candidate.snapshot.objects.end() ||
              object->scene_resource_id != attachment->scene_resource_id) {
            throw CanonicalEmptySceneOwnerIntegrityError(
                "carried attachment has no matching scene object");
          }
          const auto *carried =
              std::get_if<core::CarriedSceneObjectPlacementDTO>(
                  &object->placement);
          if (carried == nullptr ||
              carried->carried_object_attachment_id !=
                  attachment->carried_object_attachment_id ||
              carried->carried_object_attachment_revision !=
                  attachment->carried_object_attachment_revision) {
            throw CanonicalEmptySceneOwnerIntegrityError(
                "carried scene object does not identify its attachment");
          }
          const auto removed_id = attachment->carried_object_attachment_id;
          const auto removed_revision =
              attachment->carried_object_attachment_revision;
          object->placement = core::CellFixedSceneObjectPlacementDTO{
              current.cell_id,
              core::multiply_poses(frame_resolution->frame_pose_in_cell,
                                   attachment->pose_in_carrier_frame)};
          candidate.snapshot.carried_object_attachments.erase(attachment);
          candidate.operation = core::ResolvedDetachCarriedObjectDTO{
              *object, removed_id, removed_revision};
          candidate.resolution_evidence_digest =
              frame_resolution->resolution_evidence_digest;
          policy_changed = true;
        } else {
          topology_changed = false;
          const auto &current_policy = current.configured_collision_policy;
          if (operation.expected_configured_policy_revision.revision_id !=
                  current_policy.revision.revision_id ||
              operation.expected_configured_policy_revision.content_hash !=
                  current_policy.revision.content_hash) {
            throw SceneRequestRejectionError(
                "stale_revision",
                "configured collision-policy precondition failed",
                {{"expected_collision_policy_revision",
                  operation.expected_configured_policy_revision.content_hash},
                 {"current_collision_policy_revision",
                  current_policy.revision.content_hash}});
          }
          if (!operation.configured_policy.allowed_static_static_pairs
                   .empty() ||
              !operation.configured_policy.allowed_touch_link_pairs.empty() ||
              !operation.configured_policy.support_contacts.empty()) {
            throw SceneRequestRejectionError(
                "invalid_request",
                "configured collision policy cannot contain owner-derived "
                "static, touch, or support allowances",
                {{"reason", "derived_collision_policy_fields_are_read_only"}});
          }
          auto next_policy = operation.configured_policy;
          const auto digest =
              core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
                  next_policy);
          if (digest.value() == current_policy.revision.content_hash) {
            next_policy.revision = current_policy.revision;
            candidate.update_status = SceneUpdateStatusDTO::kNoOp;
            content_expected_to_change = false;
          } else {
            if (current_policy.revision.revision_id >= kMaximumSafeIntegral) {
              throw CanonicalEmptySceneOwnerLimitError(
                  "collision-policy revision is exhausted");
            }
            next_policy.revision = {
                current_policy.revision.revision_id + 1U,
                digest.value(),
            };
            policy_changed = true;
          }
          candidate.snapshot.configured_collision_policy = next_policy;
          candidate.operation = core::ResolvedCollisionPolicyDTO{
              next_policy,
              std::move(next_policy),
          };
        }
      },
      request.operation);

  if (content_expected_to_change) {
    if (candidate.snapshot.revision.revision_id >= kMaximumSafeIntegral) {
      throw CanonicalEmptySceneOwnerLimitError(
          "logical-scene revision is exhausted");
    }
    candidate.snapshot.revision.revision_id += 1U;
    if (topology_changed) {
      if (candidate.snapshot.topology_epoch >= kMaximumSafeIntegral) {
        throw CanonicalEmptySceneOwnerLimitError(
            "logical-scene topology epoch is exhausted");
      }
      candidate.snapshot.topology_epoch += 1U;
    }
    if (policy_changed) {
      if (candidate.snapshot.policy_epoch >= kMaximumSafeIntegral) {
        throw CanonicalEmptySceneOwnerLimitError(
            "logical-scene policy epoch is exhausted");
      }
      candidate.snapshot.policy_epoch += 1U;
    }
    candidate.snapshot.revision.content_hash =
        core::logical_scene_identity::ComputeLogicalSceneContentHash(
            candidate.snapshot);
  }
  if (!candidate.snapshot.locally_valid()) {
    throw std::invalid_argument(
        "scene operation does not form a valid logical snapshot");
  }
  return candidate;
}

void InsertSceneIntent(sqlite3 *database,
                       const SceneTransactionRequestDTO &request,
                       const ContentDigest &request_digest,
                       const std::string_view request_json,
                       const core::SceneTransactionDTO &transaction,
                       const ResolvedSceneCandidate &candidate,
                       const std::size_t required_projection_count) {
  const auto resolved_json =
      logical_scene_codec_internal::EncodeResolvedOperation(
          transaction.resolved_operation);
  const auto snapshot_json =
      logical_scene_codec_internal::EncodeSnapshot(candidate.snapshot);
  if (request_json.size() > kMaximumLogicalSceneTransactionBytes ||
      resolved_json.size() > kMaximumLogicalSceneTransactionBytes ||
      snapshot_json.size() > kMaximumLogicalSceneTransactionBytes) {
    throw CanonicalEmptySceneOwnerLimitError(
        "logical-scene transaction exceeds its durable byte bound");
  }
  Statement statement(
      database,
      "INSERT INTO logical_scene_transaction_intents("
      "transaction_sequence,request_owner_epoch,request_id,request_digest,"
      "request_json,prior_retained_bundle_id,prior_graph_revision,"
      "prior_scene_id,prior_scene_revision_id,prior_scene_content_hash,"
      "proposed_retained_bundle_id,proposed_graph_revision,"
      "proposed_scene_id,proposed_scene_revision_id,"
      "proposed_scene_content_hash,resolved_operation_json,"
      "resolution_evidence_digest,candidate_snapshot_json,update_status,"
      "required_projection_count) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  statement.Bind(1, CheckedSafeCounter(transaction.transaction_sequence,
                                       "transaction_sequence"));
  statement.Bind(2, request.request_context.request_owner_epoch.value());
  statement.Bind(3, request.request_context.request_id.value());
  statement.Bind(4, request_digest.value());
  statement.Bind(5, request_json);
  statement.Bind(
      6, transaction.prior_projection_revision.retained_bundle_id.value());
  statement.Bind(7,
                 transaction.prior_projection_revision.graph_revision.value());
  statement.Bind(
      8, transaction.prior_projection_revision.scene_revision.scene_id.value());
  statement.Bind(
      9, CheckedSafeCounter(
             transaction.prior_projection_revision.scene_revision.revision_id,
             "prior scene revision"));
  statement.Bind(10, transaction.prior_projection_revision.scene_revision
                         .content_hash.value());
  statement.Bind(
      11, transaction.proposed_projection_revision.retained_bundle_id.value());
  statement.Bind(
      12, transaction.proposed_projection_revision.graph_revision.value());
  statement.Bind(
      13,
      transaction.proposed_projection_revision.scene_revision.scene_id.value());
  statement.Bind(
      14,
      CheckedSafeCounter(
          transaction.proposed_projection_revision.scene_revision.revision_id,
          "proposed scene revision"));
  statement.Bind(15, transaction.proposed_projection_revision.scene_revision
                         .content_hash.value());
  statement.Bind(16, resolved_json);
  if (transaction.resolution_evidence_digest.has_value()) {
    statement.Bind(17, transaction.resolution_evidence_digest->value());
  } else {
    statement.BindNull(17);
  }
  statement.Bind(18, snapshot_json);
  statement.Bind(19, core::to_string(candidate.update_status));
  statement.Bind(20, static_cast<std::int64_t>(required_projection_count));
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "scene intent insert unexpectedly returned a row");
  }
}

std::uint64_t NextSceneOwnerEventSequence(sqlite3 *database) {
  const auto history = ReadBoundedHistoryState(database);
  Statement statement(
      database, "SELECT count(*),coalesce(max(owner_event_sequence),0) FROM "
                "logical_scene_owner_events");
  if (!statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene owner event aggregate is missing");
  }
  const auto count = statement.Integer(0);
  const auto maximum = statement.Integer(1);
  if (statement.Step() || count < 0 ||
      count >= static_cast<std::int64_t>(kMaximumLogicalSceneOwnerEvents) ||
      maximum < 0) {
    throw CanonicalEmptySceneOwnerLimitError(
        "logical-scene owner event ledger is exhausted or invalid");
  }
  const auto watermark =
      std::max(static_cast<std::uint64_t>(maximum),
               history.compacted_through_owner_event_sequence);
  if (watermark >= kMaximumSafeIntegral) {
    throw CanonicalEmptySceneOwnerLimitError(
        "logical-scene owner event sequence is exhausted");
  }
  return watermark + 1U;
}

RequestOwnerEpoch
CompactSceneTransactionHistory(sqlite3 *database,
                               const DurableState &durable_state,
                               const SceneTransactionStoreState &transactions) {
  const auto access = AccessStatus(durable_state);
  if (!access.scene_writes_open || !access.projection_leases_open ||
      durable_state.OpenNamedApply() != nullptr ||
      durable_state.projection_desynchronized ||
      durable_state.unresolved_scene_transaction ||
      transactions.projection_desynchronized ||
      transactions.unresolved_native_attempt ||
      transactions.resident_transaction_count <
          kMaximumLogicalSceneTransactions ||
      transactions.next_transaction_sequence <= 1U ||
      transactions.next_owner_event_sequence <= 1U) {
    throw CanonicalEmptySceneOwnerConflictError(
        "logical-scene history compaction requires a settled quiescent head");
  }
  for (const auto &stage : durable_state.named_apply_stages) {
    if (!stage.discarded && !stage.finalization_receipt.has_value()) {
      throw CanonicalEmptySceneOwnerConflictError(
          "logical-scene history compaction cannot capture an open Apply");
    }
  }
  const auto history = ReadBoundedHistoryState(database);
  if (history.compaction_generation >= kMaximumSafeIntegral ||
      history.request_epoch_generation >= kMaximumSafeIntegral) {
    throw CanonicalEmptySceneOwnerLimitError(
        "logical-scene history compaction generation is exhausted");
  }
  const auto next_compaction_generation = history.compaction_generation + 1U;
  const auto next_request_epoch_generation =
      history.request_epoch_generation == 0U
          ? 2U
          : history.request_epoch_generation + 1U;
  const auto next_request_owner_epoch =
      CompactedRequestOwnerEpoch(transactions.active_snapshot.revision.scene_id,
                                 next_request_epoch_generation);
  const auto snapshot_json = logical_scene_codec_internal::EncodeSnapshot(
      transactions.active_snapshot);
  if (snapshot_json.empty() ||
      snapshot_json.size() > kMaximumLogicalSceneTransactionBytes) {
    throw CanonicalEmptySceneOwnerLimitError(
        "logical-scene checkpoint exceeds its canonical byte bound");
  }

  Statement update(
      database,
      "UPDATE logical_scene_history_state SET compaction_generation=?,"
      "request_epoch_generation=?,request_owner_epoch=?,"
      "compacted_through_transaction_sequence=?,"
      "compacted_through_owner_event_sequence=?,"
      "compacted_named_apply_stage_count=?,"
      "checkpoint_retained_bundle_id=?,checkpoint_graph_revision=?,"
      "checkpoint_scene_id=?,checkpoint_scene_revision_id=?,"
      "checkpoint_scene_content_hash=?,checkpoint_snapshot_json=?,"
      "compaction_in_progress=1 WHERE singleton=1 AND "
      "compaction_in_progress=0");
  update.Bind(1, CheckedSafeCounter(next_compaction_generation,
                                    "compaction generation"));
  update.Bind(2, CheckedSafeCounter(next_request_epoch_generation,
                                    "request epoch generation"));
  update.Bind(3, next_request_owner_epoch.value());
  update.Bind(4, CheckedSafeCounter(transactions.next_transaction_sequence - 1U,
                                    "compacted transaction sequence"));
  update.Bind(5, CheckedSafeCounter(transactions.next_owner_event_sequence - 1U,
                                    "compacted owner event sequence"));
  update.Bind(
      6, static_cast<std::int64_t>(durable_state.named_apply_stages.size()));
  update.Bind(7, transactions.active_projection.retained_bundle_id.value());
  update.Bind(8, transactions.active_projection.graph_revision.value());
  update.Bind(9,
              transactions.active_projection.scene_revision.scene_id.value());
  update.Bind(10, CheckedSafeCounter(
                      transactions.active_projection.scene_revision.revision_id,
                      "checkpoint scene revision"));
  update.Bind(
      11, transactions.active_projection.scene_revision.content_hash.value());
  update.Bind(12, snapshot_json);
  if (update.Step() || sqlite3_changes(database) != 1) {
    throw CanonicalEmptySceneOwnerConflictError(
        "logical-scene history compaction state changed concurrently");
  }

  Exec(database, "DELETE FROM logical_scene_projection_results");
  Exec(database, "DELETE FROM logical_scene_projection_attempts");
  Exec(database, "DELETE FROM logical_scene_request_outcomes");
  Exec(database, "DELETE FROM logical_scene_owner_events");
  Exec(database, "DELETE FROM logical_scene_transaction_intents");
  Exec(database, "UPDATE logical_scene_history_state SET "
                 "compaction_in_progress=0 WHERE singleton=1");

  const auto compacted =
      ReadSceneTransactionStoreState(database, durable_state);
  if (compacted.active_projection != transactions.active_projection ||
      compacted.active_snapshot != transactions.active_snapshot ||
      compacted.request_owner_epoch != next_request_owner_epoch ||
      compacted.next_transaction_sequence !=
          transactions.next_transaction_sequence ||
      compacted.next_owner_event_sequence !=
          transactions.next_owner_event_sequence ||
      compacted.resident_transaction_count != 0U) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene history compaction changed authoritative state");
  }
  return next_request_owner_epoch;
}

void InsertSceneTransactionOwnerEvent(
    sqlite3 *database, const std::uint64_t owner_event_sequence,
    const std::uint64_t transaction_sequence) {
  Statement statement(
      database,
      "INSERT INTO logical_scene_owner_events(owner_event_sequence,"
      "event_kind,transaction_sequence,named_apply_lineage_generation) "
      "VALUES(?,'scene_transaction_intent',?,NULL)");
  statement.Bind(
      1, CheckedSafeCounter(owner_event_sequence, "owner_event_sequence"));
  statement.Bind(
      2, CheckedSafeCounter(transaction_sequence, "transaction_sequence"));
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "scene transaction owner event insert unexpectedly returned a row");
  }
}

void InsertNamedApplyOwnerEvent(sqlite3 *database,
                                const std::uint64_t owner_event_sequence,
                                const std::uint64_t lineage_generation) {
  Statement statement(
      database,
      "INSERT INTO logical_scene_owner_events(owner_event_sequence,"
      "event_kind,transaction_sequence,named_apply_lineage_generation) "
      "VALUES(?,'named_apply_finalized',NULL,?)");
  statement.Bind(
      1, CheckedSafeCounter(owner_event_sequence, "owner_event_sequence"));
  statement.Bind(2, CheckedSafeCounter(lineage_generation,
                                       "named Apply lineage generation"));
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply owner event insert unexpectedly returned a row");
  }
}

void InsertSceneOutcome(sqlite3 *database,
                        const std::string_view request_owner_epoch,
                        const RequestId &request_id,
                        const ContentDigest &request_digest,
                        const std::optional<std::uint64_t> transaction_sequence,
                        const bool success,
                        const SceneProjectionCommitStatusDTO projection_status,
                        const std::size_t attempted_projection_count,
                        const std::optional<ErrorDTO> &error) {
  Statement statement(
      database,
      "INSERT INTO logical_scene_request_outcomes("
      "request_owner_epoch,request_id,request_digest,transaction_sequence,"
      "success,projection_status,attempted_projection_count,error_code,"
      "error_message,error_details_json) VALUES(?,?,?,?,?,?,?,?,?,?)");
  statement.Bind(1, request_owner_epoch);
  statement.Bind(2, request_id.value());
  statement.Bind(3, request_digest.value());
  if (transaction_sequence.has_value()) {
    statement.Bind(
        4, CheckedSafeCounter(*transaction_sequence, "transaction_sequence"));
  } else {
    statement.BindNull(4);
  }
  statement.Bind(5, success ? 1 : 0);
  statement.Bind(6, core::to_string(projection_status));
  statement.Bind(7, static_cast<std::int64_t>(attempted_projection_count));
  if (error.has_value()) {
    statement.Bind(8, error->code);
    statement.Bind(9, error->message);
    statement.Bind(
        10, logical_scene_codec_internal::EncodeErrorDetails(error->details));
  } else {
    statement.BindNull(8);
    statement.BindNull(9);
    statement.BindNull(10);
  }
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "scene outcome insert unexpectedly returned a row");
  }
}

void InsertProjectionAttempt(
    sqlite3 *database, const std::uint64_t transaction_sequence,
    const std::size_t ordinal,
    const core::ProjectionNativeRuntimeBindingDTO &binding) {
  Statement statement(
      database, "INSERT INTO logical_scene_projection_attempts("
                "transaction_sequence,projection_ordinal,provider_binding_id,"
                "provider_binding_revision,runtime_unit_id,runtime_epoch) "
                "VALUES(?,?,?,?,?,?)");
  statement.Bind(
      1, CheckedSafeCounter(transaction_sequence, "transaction_sequence"));
  statement.Bind(2, static_cast<std::int64_t>(ordinal));
  statement.Bind(3, binding.provider_binding_id.value());
  statement.Bind(4, binding.provider_binding_revision.value());
  statement.Bind(5, binding.runtime_unit_id.value());
  statement.Bind(6, binding.runtime_epoch.value());
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "projection attempt insert unexpectedly returned a row");
  }
}

std::string_view
ProjectionResultState(const core::ProjectionCommitResultStateDTO state) {
  switch (state) {
  case core::ProjectionCommitResultStateDTO::kCommitted:
    return "committed";
  case core::ProjectionCommitResultStateDTO::kRejectedRollbackProven:
    return "rejected_rollback_proven";
  case core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain:
    return "failed_projection_uncertain";
  }
  return {};
}

void InsertProjectionResult(sqlite3 *database,
                            const std::uint64_t transaction_sequence,
                            const std::size_t ordinal,
                            const core::ProjectionCommitResultDTO &result) {
  Statement statement(
      database,
      "INSERT INTO logical_scene_projection_results("
      "transaction_sequence,projection_ordinal,result_state,"
      "acknowledged_retained_bundle_id,acknowledged_graph_revision,"
      "acknowledged_scene_id,acknowledged_scene_revision_id,"
      "acknowledged_scene_content_hash,rollback_proven,"
      "resulting_health_state,error_code,error_message,error_details_json) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
  statement.Bind(
      1, CheckedSafeCounter(transaction_sequence, "transaction_sequence"));
  statement.Bind(2, static_cast<std::int64_t>(ordinal));
  statement.Bind(3, ProjectionResultState(result.result));
  if (result.acknowledged_projection_revision.has_value()) {
    const auto &ack = *result.acknowledged_projection_revision;
    statement.Bind(4, ack.retained_bundle_id.value());
    statement.Bind(5, ack.graph_revision.value());
    statement.Bind(6, ack.scene_revision.scene_id.value());
    statement.Bind(7, CheckedSafeCounter(ack.scene_revision.revision_id,
                                         "acknowledged scene revision"));
    statement.Bind(8, ack.scene_revision.content_hash.value());
  } else {
    for (int index = 4; index <= 8; ++index) {
      statement.BindNull(index);
    }
  }
  statement.Bind(9, result.rollback_proven ? 1 : 0);
  statement.Bind(10, core::to_string(result.resulting_health.state));
  if (result.error.has_value()) {
    statement.Bind(11, result.error->code);
    statement.Bind(12, result.error->message);
    statement.Bind(13, logical_scene_codec_internal::EncodeErrorDetails(
                           result.error->details));
  } else {
    statement.BindNull(11);
    statement.BindNull(12);
    statement.BindNull(13);
  }
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "projection result insert unexpectedly returned a row");
  }
}

ProjectionRevisionDTO ReadProposedProjection(sqlite3 *database,
                                             const std::uint64_t sequence) {
  Statement statement(
      database,
      "SELECT proposed_retained_bundle_id,proposed_graph_revision,"
      "proposed_scene_id,proposed_scene_revision_id,"
      "proposed_scene_content_hash FROM logical_scene_transaction_intents "
      "WHERE transaction_sequence=? LIMIT 2");
  statement.Bind(1, CheckedSafeCounter(sequence, "transaction_sequence"));
  if (!statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "scene result references an absent transaction intent");
  }
  auto result = ReadProjectionColumns(statement, 0);
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "scene transaction sequence is not unique");
  }
  return result;
}

SceneUpdateStatusDTO ReadStoredUpdateStatus(sqlite3 *database,
                                            const std::uint64_t sequence) {
  Statement statement(
      database,
      "SELECT update_status FROM logical_scene_transaction_intents WHERE "
      "transaction_sequence=? LIMIT 2");
  statement.Bind(1, CheckedSafeCounter(sequence, "transaction_sequence"));
  if (!statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "scene result update status is absent");
  }
  const auto result = DecodeUpdateStatus(statement.Text(0));
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "scene result update status is not unique");
  }
  return result;
}

SceneCommitResultDTO
ResultFromStoredOutcome(sqlite3 *database, const StoredSceneOutcome &outcome,
                        const core::RequestReplayStatusDTO replay_status) {
  if (!outcome.success) {
    return FailedSceneResult(outcome.request_id, replay_status, *outcome.error,
                             outcome.projection_status);
  }
  const auto sequence = *outcome.transaction_sequence;
  SceneCommitResultDTO result{
      outcome.request_id,
      replay_status,
      true,
      std::nullopt,
      sequence,
      ReadProposedProjection(database, sequence),
      SceneProjectionCommitStatusDTO::kCommitted,
      ReadStoredUpdateStatus(database, sequence),
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored scene outcome cannot form a valid public result");
  }
  return result;
}

struct RequiredSceneProjection final {
  std::shared_ptr<core::IPlannerSceneProjection> projection;
  core::ProjectionHealthDTO preflight_health;
};

using ProjectionProviderBindingKey =
    std::pair<core::CapabilityProviderBindingId,
              core::CapabilityProviderBindingRevision>;

using ProjectionProviderRuntimeRequirements =
    std::map<ProjectionProviderBindingKey, core::RuntimeUnitId>;

[[nodiscard]] ProjectionProviderRuntimeRequirements
NativeSceneProviderBindings(const RetainedBundleDTO &bundle) {
  std::set<core::RuntimeUnitId> declared_runtime_units;
  for (const auto &runtime_unit : bundle.cell_graph.runtime_units) {
    if (!declared_runtime_units.insert(runtime_unit.runtime_unit_id).second) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "verified retained bundle repeats a runtime-unit identity");
    }
  }
  ProjectionProviderRuntimeRequirements result;
  for (const auto &binding : bundle.capability_provider_bindings) {
    if (!core::RequiresNativeSceneProjection(
            binding.capability_id, binding.capability_definition_revision)) {
      continue;
    }
    std::set<core::RuntimeUnitId> runtime_sources;
    for (const auto &endpoint : binding.endpoint_bindings) {
      if (endpoint.source_kind !=
          core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *runtime_unit =
          std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
      if (runtime_unit == nullptr || runtime_unit->empty()) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "verified native provider has a malformed runtime endpoint");
      }
      runtime_sources.insert(*runtime_unit);
    }
    if (runtime_sources.size() != 1U ||
        !declared_runtime_units.contains(*runtime_sources.begin())) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "verified native provider does not resolve one declared runtime "
          "unit");
    }
    const auto inserted = result.emplace(
        ProjectionProviderBindingKey{
            binding.capability_provider_binding_id,
            binding.capability_provider_binding_revision},
        *runtime_sources.begin());
    if (!inserted.second) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "verified retained bundle repeats a native scene provider binding");
    }
  }
  return result;
}

[[nodiscard]] bool TransitionRowsMatchVerifiedBundles(
    const std::vector<core::ProjectionTransitionRowDTO> &rows,
    const RetainedBundleDTO &predecessor, const RetainedBundleDTO &candidate) {
  const auto predecessor_bindings = NativeSceneProviderBindings(predecessor);
  const auto candidate_bindings = NativeSceneProviderBindings(candidate);
  std::set<ProjectionProviderBindingKey> observed_predecessor;
  std::set<ProjectionProviderBindingKey> observed_candidate;
  for (const auto &row : rows) {
    const ProjectionProviderBindingKey key{
        row.native_runtime_binding.provider_binding_id,
        row.native_runtime_binding.provider_binding_revision};
    const auto predecessor_requirement = predecessor_bindings.find(key);
    const auto candidate_requirement = candidate_bindings.find(key);
    const bool matches_predecessor =
        predecessor_requirement != predecessor_bindings.end() &&
        predecessor_requirement->second ==
            row.native_runtime_binding.runtime_unit_id;
    const bool matches_candidate =
        candidate_requirement != candidate_bindings.end() &&
        candidate_requirement->second ==
            row.native_runtime_binding.runtime_unit_id;
    switch (row.disposition) {
    case core::ProjectionTransitionDisposition::kRetained:
      if (!matches_predecessor || !matches_candidate ||
          !observed_predecessor.insert(key).second ||
          !observed_candidate.insert(key).second) {
        return false;
      }
      break;
    case core::ProjectionTransitionDisposition::kAdded:
      if (!matches_candidate || !observed_candidate.insert(key).second) {
        return false;
      }
      break;
    case core::ProjectionTransitionDisposition::kRemoved:
      if (!matches_predecessor || !observed_predecessor.insert(key).second) {
        return false;
      }
      break;
    }
  }
  const auto exact_keys = [](const auto &requirements) {
    std::set<ProjectionProviderBindingKey> keys;
    for (const auto &[key, unused] : requirements) {
      static_cast<void>(unused);
      keys.insert(key);
    }
    return keys;
  };
  return observed_predecessor == exact_keys(predecessor_bindings) &&
         observed_candidate == exact_keys(candidate_bindings);
}

struct NativeTransitionParticipant final {
  core::ProjectionTransitionRowDTO transition_row;
  std::shared_ptr<core::IPlannerSceneProjection> projection;
  core::ProjectionHealthDTO observed_health;
};

[[nodiscard]] RequiredSceneProjection ResolveRequiredProjection(
    const core::ProjectionNativeRuntimeBindingDTO &binding,
    const core::IPlannerSceneProjectionResolver *resolver) {
  if (resolver == nullptr) {
    throw CanonicalEmptySceneOwnerConflictError(
        "native projection requires an installed projection resolver");
  }
  core::Result<std::shared_ptr<core::IPlannerSceneProjection>> resolved =
      [&]() {
        try {
          return resolver->ResolveProjection(binding);
        } catch (const std::exception &error) {
          return core::Result<std::shared_ptr<core::IPlannerSceneProjection>>::
              Err(core::ErrorDTO{
                  "native_projection_resolution_failed", error.what(), {}});
        } catch (...) {
          return core::Result<std::shared_ptr<core::IPlannerSceneProjection>>::
              Err(core::ErrorDTO{
                  "native_projection_resolution_failed",
                  "projection resolver threw a non-standard exception",
                  {}});
        }
      }();
  if (!resolved.ok() || !resolved.value()) {
    const std::string detail =
        resolved.ok() ? "projection resolver returned a null client"
                      : resolved.error().message;
    throw CanonicalEmptySceneOwnerConflictError(
        "native projection is unavailable: " + detail);
  }
  auto projection = std::move(resolved.value());
  auto health = projection->ProjectionHealth();
  if (!health.valid() || health.source_sequence == 0U) {
    throw CanonicalEmptySceneOwnerConflictError(
        "native projection returned malformed health");
  }
  if (health.native_runtime_binding != binding) {
    throw CanonicalEmptySceneOwnerConflictError(
        "resolved native projection differs from the selected host "
        "incarnation");
  }
  return {std::move(projection), std::move(health)};
}

class ScopedProjectionLease final {
public:
  ScopedProjectionLease(
      std::shared_ptr<core::IPlannerSceneProjection> projection,
      core::ProjectionLeaseId lease_id)
      : projection_(std::move(projection)), lease_id_(std::move(lease_id)) {}

  ScopedProjectionLease(const ScopedProjectionLease &) = delete;
  ScopedProjectionLease &operator=(const ScopedProjectionLease &) = delete;
  ScopedProjectionLease(ScopedProjectionLease &&other) noexcept
      : projection_(std::move(other.projection_)),
        lease_id_(std::move(other.lease_id_)) {}
  ScopedProjectionLease &operator=(ScopedProjectionLease &&) = delete;

  void Release() {
    if (!projection_ || lease_id_.empty()) {
      return;
    }
    projection_->ReleaseProjection(lease_id_);
    projection_.reset();
    lease_id_ = core::ProjectionLeaseId(std::string{});
  }

  ~ScopedProjectionLease() {
    if (!projection_ || lease_id_.empty()) {
      return;
    }
    try {
      projection_->ReleaseProjection(lease_id_);
    } catch (...) {
      // Resolution already captured an immutable receipt. Release failure is
      // bounded native cleanup and must not forge a different scene result.
    }
  }

private:
  std::shared_ptr<core::IPlannerSceneProjection> projection_;
  core::ProjectionLeaseId lease_id_;
};

[[nodiscard]] std::optional<core::SceneFrameResolutionSelectorDTO>
FrameSelectorForRequest(const SceneTransactionRequestDTO &request,
                        const LogicalSceneSnapshotDTO &current) {
  return std::visit(
      [&current](const auto &operation)
          -> std::optional<core::SceneFrameResolutionSelectorDTO> {
        using T = std::remove_cvref_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, core::AddObjectRequestDTO> ||
                      std::is_same_v<T, core::UpsertObjectRequestDTO> ||
                      std::is_same_v<T, core::UpdateObjectPoseRequestDTO> ||
                      std::is_same_v<T, core::DetachCarriedObjectRequestDTO>) {
          return std::visit(
              [&current](const auto &frame)
                  -> std::optional<core::SceneFrameResolutionSelectorDTO> {
                using Frame = std::remove_cvref_t<decltype(frame)>;
                if constexpr (std::is_same_v<Frame,
                                             core::CellSceneFrameRefDTO>) {
                  return std::nullopt;
                } else if constexpr (std::is_same_v<
                                         Frame,
                                         core::SceneResourceSceneFrameRefDTO>) {
                  const auto object = std::lower_bound(
                      current.objects.begin(), current.objects.end(),
                      frame.scene_resource_id,
                      [](const core::SceneObjectSnapshotDTO &entry,
                         const core::SceneResourceId &id) {
                        return entry.scene_resource_id < id;
                      });
                  if (object == current.objects.end() ||
                      object->scene_resource_id != frame.scene_resource_id) {
                    return std::nullopt;
                  }
                  const auto *carried =
                      std::get_if<core::CarriedSceneObjectPlacementDTO>(
                          &object->placement);
                  if (carried == nullptr) {
                    return std::nullopt;
                  }
                  const auto attachment = std::lower_bound(
                      current.carried_object_attachments.begin(),
                      current.carried_object_attachments.end(),
                      carried->carried_object_attachment_id,
                      [](const core::CarriedObjectAttachmentDTO &entry,
                         const core::CarriedObjectAttachmentId &id) {
                        return entry.carried_object_attachment_id < id;
                      });
                  if (attachment ==
                          current.carried_object_attachments.end() ||
                      attachment->carried_object_attachment_id !=
                          carried->carried_object_attachment_id ||
                      attachment->carried_object_attachment_revision !=
                          carried->carried_object_attachment_revision) {
                    throw CanonicalEmptySceneOwnerIntegrityError(
                        "carried scene-resource selector has no exact "
                        "attachment row");
                  }
                  return core::SceneFrameResolutionSelectorDTO{
                      std::in_place_type<core::SceneFrameRefDTO>,
                      attachment->carrier_frame_ref};
                } else {
                  return core::SceneFrameResolutionSelectorDTO{
                      std::in_place_type<core::SceneFrameRefDTO>,
                      core::SceneFrameRefDTO{frame}};
                }
              },
              operation.frame_ref);
        } else if constexpr (std::is_same_v<
                                 T, core::AttachCarriedObjectRequestDTO>) {
          return core::SceneFrameResolutionSelectorDTO{
              core::TargetCarrierSceneFrameRefDTO{
                  operation.controllable_component_id,
                  operation.command_target_id, operation.frame_role_id}};
        } else if constexpr (
            std::is_same_v<T,
                           core::DetachCarriedObjectAtCurrentPoseRequestDTO>) {
          const auto attachment =
              std::lower_bound(current.carried_object_attachments.begin(),
                               current.carried_object_attachments.end(),
                               operation.carried_object_attachment_id,
                               [](const core::CarriedObjectAttachmentDTO &entry,
                                  const core::CarriedObjectAttachmentId &id) {
                                 return entry.carried_object_attachment_id < id;
                               });
          if (attachment == current.carried_object_attachments.end() ||
              attachment->carried_object_attachment_id !=
                  operation.carried_object_attachment_id) {
            return std::nullopt;
          }
          return core::SceneFrameResolutionSelectorDTO{
              std::in_place_type<core::SceneFrameRefDTO>,
              attachment->carrier_frame_ref};
        } else {
          return std::nullopt;
        }
      },
      request.operation);
}

[[nodiscard]] bool SameResolutionMeaning(
    const core::SceneFrameResolutionReceiptDTO &left,
    const core::SceneFrameResolutionReceiptDTO &right) noexcept {
  return left.resolved_frame == right.resolved_frame &&
         SamePose(left.frame_pose_in_cell, right.frame_pose_in_cell) &&
         left.component_attachment_revisions ==
             right.component_attachment_revisions;
}

[[nodiscard]] bool ProjectionDoesNotOwnFrame(const ErrorDTO &error) noexcept {
  if (error.code != "missing_reference") {
    return false;
  }
  const auto reason = error.details.find("reason");
  return reason != error.details.end() &&
         (reason->second == "scene_frame_reference_missing" ||
          reason->second == "kinematic_state_source_unavailable");
}

[[nodiscard]] std::optional<core::SceneFrameResolutionReceiptDTO>
ResolveFrameAcrossProjections(
    const core::SceneFrameResolutionSelectorDTO &selector,
    const ProjectionRevisionDTO &projection_revision,
    const std::vector<RequiredSceneProjection> &projections,
    const std::string_view lease_scope) {
  if (projections.empty()) {
    throw SceneRequestRejectionError(
        "missing_reference",
        "scene-frame resolution has no active native projection",
        {{"reason", "scene_frame_projection_unavailable"}});
  }

  std::vector<ScopedProjectionLease> leases;
  leases.reserve(projections.size());
  std::vector<core::SceneFrameResolutionReceiptDTO> resolutions;
  resolutions.reserve(projections.size());
  std::optional<ErrorDTO> first_missing;
  for (std::size_t ordinal = 0U; ordinal < projections.size(); ++ordinal) {
    const auto &projection = projections[ordinal];
    core::ProjectionLeaseId lease_id("scene-resolve-" +
                                     std::string(lease_scope) + "-" +
                                     std::to_string(ordinal));
    auto retained = projection.projection->RetainProjectionBinding(
        projection_revision, lease_id);
    if (!retained.ok()) {
      throw SceneRequestRejectionError(retained.error().code,
                                       retained.error().message,
                                       retained.error().details);
    }
    const core::ProjectionLeaseBindingDTO expected_binding{projection_revision,
                                                           lease_id};
    if (retained.value() != expected_binding) {
      projection.projection->ReleaseProjection(lease_id);
      throw SceneRequestRejectionError(
          "live_scene_snapshot_unavailable",
          "native projection retained a different scene revision",
          {{"reason", "scene_frame_lease_binding_mismatch"}});
    }
    leases.emplace_back(projection.projection, lease_id);
    auto resolved = projection.projection->ResolveSceneFrame(
        core::SceneFrameResolutionQueryDTO{selector, expected_binding});
    if (!resolved.ok()) {
      if (!ProjectionDoesNotOwnFrame(resolved.error())) {
        throw SceneRequestRejectionError(resolved.error().code,
                                         resolved.error().message,
                                         resolved.error().details);
      }
      if (!first_missing.has_value()) {
        first_missing = resolved.error();
      }
      continue;
    }
    if (!resolved.value().valid() ||
        resolved.value().projection_binding != expected_binding ||
        resolved.value().predicted_state_digest.has_value()) {
      throw SceneRequestRejectionError(
          "live_scene_snapshot_unavailable",
          "native projection returned a malformed frame receipt",
          {{"reason", "scene_frame_receipt_mismatch"}});
    }
    resolutions.push_back(std::move(resolved.value()));
  }
  if (resolutions.empty()) {
    if (first_missing.has_value()) {
      throw SceneRequestRejectionError(
          first_missing->code, first_missing->message, first_missing->details);
    }
    throw SceneRequestRejectionError(
        "missing_reference", "scene frame has no retained resolution",
        {{"reason", "scene_frame_reference_missing"}});
  }
  for (std::size_t index = 1U; index < resolutions.size(); ++index) {
    if (!SameResolutionMeaning(resolutions.front(), resolutions[index])) {
      throw SceneRequestRejectionError(
          "missing_reference",
          "active native projections disagree on scene-frame meaning",
          {{"reason", "scene_frame_resolution_ambiguous"}});
    }
  }
  for (auto &lease : leases) {
    lease.Release();
  }
  return resolutions.front();
}

[[nodiscard]] std::optional<core::SceneFrameResolutionReceiptDTO>
ResolveFrameForRequest(
    const SceneTransactionRequestDTO &request,
    const LogicalSceneSnapshotDTO &current,
    const std::vector<RequiredSceneProjection> &projections) {
  const auto selector = FrameSelectorForRequest(request, current);
  if (!selector.has_value()) {
    return std::nullopt;
  }
  return ResolveFrameAcrossProjections(
      *selector, request.expected_projection_revision, projections,
      request.request_context.request_id.value());
}

[[nodiscard]] core::CollisionPolicySnapshot ProjectEffectivePolicy(
    const LogicalSceneSnapshotDTO &candidate,
    const std::vector<RequiredSceneProjection> &projections) {
  if (projections.empty()) {
    return candidate.configured_collision_policy;
  }
  // The owner-side resolved policy is evidence for this exact immutable scene
  // candidate, so its nonzero monotonic publisher counter follows the scene
  // revision. The separately retained policy_epoch remains the semantic gate
  // used by operation leases when derived pairs change with stable intent.
  const auto effective_policy_revision_id = candidate.revision.revision_id;
  std::optional<core::CollisionPolicySnapshot> selected;
  for (const auto &projection : projections) {
    auto projected = projection.projection->ProjectEffectiveCollisionPolicy(
        candidate, effective_policy_revision_id);
    if (!projected.ok()) {
      throw SceneRequestRejectionError(projected.error().code,
                                       projected.error().message,
                                       projected.error().details);
    }
    const auto &effective = projected.value();
    if (effective.revision.revision_id != effective_policy_revision_id ||
        effective.policy_id !=
            candidate.configured_collision_policy.policy_id ||
        effective.contact_grasps !=
            candidate.configured_collision_policy.contact_grasps ||
        effective.require_full_diagnostics !=
            candidate.configured_collision_policy.require_full_diagnostics) {
      throw SceneRequestRejectionError(
          "scene_policy_projection_invalid",
          "native projection changed configured collision-policy semantics",
          {{"reason", "effective_collision_policy_contract_mismatch"}});
    }
    try {
      if (core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
              effective)
              .value() != effective.revision.content_hash) {
        throw SceneRequestRejectionError(
            "scene_policy_projection_invalid",
            "native projection returned an inexact collision-policy digest",
            {{"reason", "effective_collision_policy_digest_mismatch"}});
      }
    } catch (const SceneRequestRejectionError &) {
      throw;
    } catch (const std::exception &error) {
      throw SceneRequestRejectionError(
          "scene_policy_projection_invalid", error.what(),
          {{"reason", "effective_collision_policy_not_canonical"}});
    }
    if (!selected.has_value()) {
      selected = std::move(projected.value());
    } else if (*selected != projected.value()) {
      throw SceneRequestRejectionError(
          "scene_policy_projection_ambiguous",
          "active native projections disagree on effective collision policy",
          {{"reason", "effective_collision_policy_mismatch"}});
    }
  }
  return std::move(*selected);
}

[[nodiscard]] std::vector<NativeTransitionParticipant>
ObserveNativeTransitionParticipants(
    const std::vector<core::ProjectionTransitionRowDTO> &rows,
    const core::IPlannerSceneProjectionResolver *resolver) {
  if (!rows.empty() && resolver == nullptr) {
    throw CanonicalEmptySceneOwnerConflictError(
        "native transition requires an installed projection resolver");
  }
  std::vector<NativeTransitionParticipant> result;
  result.reserve(rows.size());
  for (const auto &row : rows) {
    auto observed =
        ResolveRequiredProjection(row.native_runtime_binding, resolver);
    result.push_back({row, std::move(observed.projection),
                      std::move(observed.preflight_health)});
  }
  return result;
}

core::ProjectionCommitResultDTO
UncertainProjectionResult(const core::SceneTransactionDTO &transaction,
                          const core::ProjectionHealthDTO &preflight_health,
                          ErrorDTO error) {
  auto unavailable = preflight_health;
  unavailable.state = ProjectionHealthState::kUnavailable;
  unavailable.staged_candidate = std::nullopt;
  unavailable.error = error;
  core::ProjectionCommitResultDTO result{
      transaction.transaction_sequence,
      transaction.proposed_projection_revision,
      core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain,
      std::nullopt,
      unavailable.native_projection_generation,
      false,
      std::move(unavailable),
      std::move(error),
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot construct a fail-closed projection result");
  }
  return result;
}

core::ProjectionCommitResultDTO
NormalizeProjectionResult(const core::SceneTransactionDTO &transaction,
                          const core::ProjectionHealthDTO &preflight_health,
                          core::ProjectionCommitResultDTO result) {
  const auto contract_error = [&] {
    return UncertainProjectionResult(
        transaction, preflight_health,
        SceneError("scene_projection_contract_violation",
                   "native scene projection returned contradictory or "
                   "incomplete acknowledgement evidence"));
  };
  if (!result.presence_valid() ||
      result.transaction_sequence != transaction.transaction_sequence ||
      result.proposed_projection_revision !=
          transaction.proposed_projection_revision ||
      result.resulting_health.native_runtime_binding !=
          preflight_health.native_runtime_binding ||
      result.resulting_health.source_sequence <
          preflight_health.source_sequence) {
    return contract_error();
  }
  switch (result.result) {
  case core::ProjectionCommitResultStateDTO::kCommitted:
    return result;
  case core::ProjectionCommitResultStateDTO::kRejectedRollbackProven:
    if (!result.resulting_health.healthy_for_target(
            transaction.prior_projection_revision) ||
        (result.acknowledged_projection_revision.has_value() &&
         *result.acknowledged_projection_revision !=
             transaction.prior_projection_revision)) {
      return contract_error();
    }
    return result;
  case core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain:
    return result;
  }
  return contract_error();
}

std::optional<AcceptedState>
ReadAcceptance(sqlite3 *database,
               const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query) {
  Statement statement(
      database,
      "SELECT bootstrap_id,bootstrap_generation,cell_id,retained_bundle_id,"
      "graph_revision,scene_id,scene_revision_id,scene_content_hash,"
      "transaction_sequence,topology_epoch,policy_epoch,configured_policy_id,"
      "configured_policy_revision_id,configured_policy_content_hash,"
      "object_count,carried_attachment_count,native_runtime_binding_count "
      "FROM canonical_empty_scene_acceptance ORDER BY singleton LIMIT 2");
  if (!statement.Step()) {
    return std::nullopt;
  }

  const auto bootstrap_id_text = statement.Text(0);
  const auto bootstrap_generation_value = statement.Integer(1);
  const auto cell_id_text = statement.Text(2);
  const auto retained_bundle_id_text = statement.Text(3);
  const auto graph_revision_text = statement.Text(4);
  const auto scene_id_text = statement.Text(5);
  const auto scene_revision_id = statement.Integer(6);
  const auto scene_content_hash = statement.Text(7);
  const auto transaction_sequence = statement.Integer(8);
  const auto topology_epoch = statement.Integer(9);
  const auto policy_epoch = statement.Integer(10);
  const auto policy_id = statement.Text(11);
  const auto policy_revision_id = statement.Integer(12);
  const auto policy_content_hash = statement.Text(13);
  const auto object_count = statement.Integer(14);
  const auto attachment_count = statement.Integer(15);
  const auto native_binding_count = statement.Integer(16);
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene acceptance cardinality exceeds one");
  }

  ValidateBoundedIdentity(bootstrap_id_text, "stored bootstrap_id");
  ValidateDigest(scene_id_text, "stored scene_id");
  ValidateDigest(scene_content_hash, "stored scene content_hash");
  ValidateDigest(policy_content_hash, "stored policy content_hash");
  if (bootstrap_generation_value <= 0 ||
      static_cast<std::uint64_t>(bootstrap_generation_value) >
          kMaximumSafeIntegral ||
      cell_id_text != fixed_query.cell_id.value() ||
      retained_bundle_id_text !=
          fixed_query.canonical_empty_retained_bundle_id.value() ||
      graph_revision_text !=
          fixed_query.canonical_empty_graph_revision.value() ||
      scene_revision_id != 1 || transaction_sequence != 0 ||
      topology_epoch != 0 || policy_epoch != 0 || policy_id != "free_space" ||
      policy_revision_id != 1 ||
      policy_content_hash != core::kCanonicalEmptyCollisionPolicyDigest ||
      object_count != 0 || attachment_count != 0 || native_binding_count != 0) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored logical-scene acceptance is not exact canonical empty");
  }

  const BootstrapId bootstrap_id(bootstrap_id_text);
  const BootstrapGeneration bootstrap_generation(
      static_cast<std::uint64_t>(bootstrap_generation_value));
  const SceneRevision scene_revision{
      SceneId(scene_id_text),
      static_cast<std::uint64_t>(scene_revision_id),
      ContentDigest(scene_content_hash),
  };
  CollisionPolicySnapshot policy;
  policy.policy_id = policy_id;
  policy.revision = {static_cast<std::uint64_t>(policy_revision_id),
                     policy_content_hash};
  LogicalSceneSnapshotDTO snapshot{
      scene_revision,
      CellId(cell_id_text),
      static_cast<std::uint64_t>(transaction_sequence),
      static_cast<std::uint64_t>(topology_epoch),
      static_cast<std::uint64_t>(policy_epoch),
      std::move(policy),
      {},
      {},
  };
  const ProjectionRevisionDTO target{
      RetainedBundleId(retained_bundle_id_text),
      GraphRevision(graph_revision_text),
      scene_revision,
  };

  try {
    const auto expected_scene_id = core::logical_scene_identity::MintSceneId(
        LogicalSceneBootstrapIdentityInputDTO{fixed_query.cell_id, bootstrap_id,
                                              bootstrap_generation});
    const auto expected_policy_hash =
        core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
            snapshot.configured_collision_policy);
    const auto expected_scene_hash =
        core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot);
    if (expected_scene_id != scene_revision.scene_id ||
        expected_policy_hash.value() != policy_content_hash ||
        expected_scene_hash != scene_revision.content_hash ||
        !snapshot.canonical_empty_for({scene_revision, fixed_query.cell_id}) ||
        !target.valid()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored canonical-empty scene identities do not match their "
          "qualified preimages");
    }
  } catch (const CanonicalEmptySceneOwnerIntegrityError &) {
    throw;
  } catch (const std::exception &error) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot verify stored canonical-empty scene identity: " +
        std::string(error.what()));
  }
  return AcceptedState{bootstrap_id, bootstrap_generation, target,
                       std::move(snapshot)};
}

bool ReadRebuild(sqlite3 *database,
                 const std::optional<AcceptedState> &accepted) {
  Statement statement(
      database,
      "SELECT bootstrap_id,bootstrap_generation,native_rebuild_result_count,"
      "aggregate_health_state,aggregate_native_health_count "
      "FROM canonical_empty_scene_rebuild ORDER BY singleton LIMIT 2");
  if (!statement.Step()) {
    return false;
  }
  const auto bootstrap_id = statement.Text(0);
  const auto bootstrap_generation = statement.Integer(1);
  const auto rebuild_count = statement.Integer(2);
  const auto health_state = statement.Text(3);
  const auto health_count = statement.Integer(4);
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene rebuild cardinality exceeds one");
  }
  if (!accepted.has_value() || bootstrap_id != accepted->bootstrap_id.value() ||
      bootstrap_generation !=
          static_cast<std::int64_t>(accepted->bootstrap_generation.value()) ||
      rebuild_count != 0 || health_state != "healthy" || health_count != 0) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored zero-native rebuild does not match its acceptance");
  }
  return true;
}

std::optional<ActiveBundlePublicationReceiptDTO>
ReadFinalization(sqlite3 *database,
                 const std::optional<AcceptedState> &accepted,
                 const bool rebuilt) {
  Statement statement(
      database,
      "SELECT bootstrap_id,bootstrap_generation,publication_generation,"
      "publication_bootstrap_id,publication_bootstrap_generation,"
      "candidate_retained_bundle_id,candidate_graph_revision,"
      "candidate_scene_id,candidate_scene_revision_id,"
      "candidate_scene_content_hash,request_candidate_retained_bundle_id,"
      "request_candidate_graph_revision,"
      "private_materialization_content_digest,"
      "retired_command_surfaces_evidence_digest,"
      "durable_public_pointer_content_digest "
      "FROM canonical_empty_scene_finalization ORDER BY singleton LIMIT 2");
  if (!statement.Step()) {
    return std::nullopt;
  }
  const auto bootstrap_id = statement.Text(0);
  const auto bootstrap_generation = statement.Integer(1);
  const auto publication_generation = statement.Integer(2);
  const auto publication_bootstrap_id = statement.Text(3);
  const auto publication_bootstrap_generation = statement.Integer(4);
  const auto candidate_retained_bundle_id = statement.Text(5);
  const auto candidate_graph_revision = statement.Text(6);
  const auto candidate_scene_id = statement.Text(7);
  const auto candidate_scene_revision_id = statement.Integer(8);
  const auto candidate_scene_content_hash = statement.Text(9);
  const auto request_retained_bundle_id = statement.Text(10);
  const auto request_graph_revision = statement.Text(11);
  const auto private_materialization_digest = statement.Text(12);
  const auto retired_surfaces_digest = statement.Text(13);
  const auto public_pointer_digest = statement.Text(14);
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene finalization cardinality exceeds one");
  }
  if (!accepted.has_value() || !rebuilt || publication_generation != 1 ||
      bootstrap_id != accepted->bootstrap_id.value() ||
      publication_bootstrap_id != bootstrap_id ||
      bootstrap_generation !=
          static_cast<std::int64_t>(accepted->bootstrap_generation.value()) ||
      publication_bootstrap_generation != bootstrap_generation ||
      candidate_retained_bundle_id !=
          accepted->target.retained_bundle_id.value() ||
      candidate_graph_revision != accepted->target.graph_revision.value() ||
      candidate_scene_id != accepted->target.scene_revision.scene_id.value() ||
      candidate_scene_revision_id !=
          static_cast<std::int64_t>(
              accepted->target.scene_revision.revision_id) ||
      candidate_scene_content_hash !=
          accepted->target.scene_revision.content_hash.value() ||
      request_retained_bundle_id != candidate_retained_bundle_id ||
      request_graph_revision != candidate_graph_revision) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored finalization receipt does not match its accepted target");
  }
  ValidateDigest(private_materialization_digest,
                 "stored private materialization digest");
  ValidateDigest(retired_surfaces_digest,
                 "stored retired command-surfaces digest");
  ValidateDigest(public_pointer_digest, "stored public-pointer digest");

  ActiveBundlePublicationRequestDTO request{
      BootstrapPublicationTransactionDTO{
          BootstrapId(publication_bootstrap_id),
          BootstrapGeneration(
              static_cast<std::uint64_t>(publication_bootstrap_generation)),
      },
      accepted->target,
      RetainedBundleId(request_retained_bundle_id),
      GraphRevision(request_graph_revision),
      std::nullopt,
      ContentDigest(private_materialization_digest),
      ContentDigest(retired_surfaces_digest),
  };
  ActiveBundlePublicationReceiptDTO receipt{
      std::move(request),
      ActiveBundlePublicationGeneration(
          static_cast<std::uint64_t>(publication_generation)),
      ContentDigest(public_pointer_digest),
  };
  if (!receipt.matches_bootstrap(accepted->bootstrap_id,
                                 accepted->bootstrap_generation,
                                 accepted->target)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored finalization receipt is structurally invalid");
  }
  return receipt;
}

struct NativeApplyIntentState final {
  std::uint64_t lineage_generation;
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration stage_generation;
  ProjectionRevisionDTO predecessor;
  ProjectionRevisionDTO candidate;
  LogicalSceneSnapshotDTO frozen_snapshot;
  std::vector<core::ProjectionTransitionRowDTO> transition_rows;
  bool activation_selected{};
};

std::vector<core::ProjectionTransitionRowDTO>
ReadNativeTransitionRows(sqlite3 *database, const ApplyId &apply_id,
                         const ApplyGeneration apply_generation,
                         const ProjectionStageGeneration stage_generation,
                         const ProjectionRevisionDTO &predecessor,
                         const std::size_t expected_count) {
  Statement statement(
      database,
      "SELECT transition_ordinal,provider_binding_id,"
      "provider_binding_revision,runtime_unit_id,runtime_epoch,disposition,"
      "expected_local_retained_bundle_id,expected_local_graph_revision,"
      "expected_local_scene_id,expected_local_scene_revision_id,"
      "expected_local_scene_content_hash "
      "FROM logical_scene_named_apply_native_transition_rows "
      "WHERE apply_id=? AND apply_generation=? AND "
      "projection_stage_generation=? ORDER BY transition_ordinal LIMIT 257");
  statement.Bind(1, apply_id.value());
  statement.Bind(2, CheckedSafeCounter(apply_generation.value(),
                                       "native transition Apply generation"));
  statement.Bind(3, CheckedSafeCounter(stage_generation.value(),
                                       "native transition stage generation"));
  std::vector<core::ProjectionTransitionRowDTO> rows;
  while (statement.Step()) {
    if (rows.size() >= kMaximumNativeProjectionTransitionRows ||
        statement.Integer(0) != static_cast<std::int64_t>(rows.size())) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native transition row order or cardinality is invalid");
    }
    const auto provider_binding_id = statement.Text(1);
    const auto provider_binding_revision = statement.Text(2);
    const auto runtime_unit_id = statement.Text(3);
    const auto runtime_epoch = statement.Text(4);
    ValidateBoundedIdentity(provider_binding_id,
                            "stored projection provider binding ID");
    ValidateBoundedIdentity(provider_binding_revision,
                            "stored projection provider binding revision");
    ValidateBoundedIdentity(runtime_unit_id,
                            "stored projection runtime unit ID");
    ValidateBoundedIdentity(runtime_epoch, "stored projection runtime epoch");
    core::ProjectionTransitionDisposition disposition;
    const auto disposition_text = statement.Text(5);
    if (disposition_text == "retained") {
      disposition = core::ProjectionTransitionDisposition::kRetained;
    } else if (disposition_text == "added") {
      disposition = core::ProjectionTransitionDisposition::kAdded;
    } else if (disposition_text == "removed") {
      disposition = core::ProjectionTransitionDisposition::kRemoved;
    } else {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored native transition disposition is invalid");
    }
    const bool expected_absent = statement.IsNull(6);
    for (int column = 6; column <= 10; ++column) {
      if (statement.IsNull(column) != expected_absent) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "stored native transition predecessor has partial presence");
      }
    }
    std::optional<ProjectionRevisionDTO> expected_local;
    if (!expected_absent) {
      const auto bundle = statement.Text(6);
      const auto graph = statement.Text(7);
      const auto scene_id = statement.Text(8);
      const auto scene_revision = statement.Integer(9);
      const auto scene_hash = statement.Text(10);
      ValidateDigest(bundle, "stored native predecessor bundle");
      ValidateDigest(graph, "stored native predecessor graph");
      ValidateDigest(scene_id, "stored native predecessor scene ID");
      ValidateDigest(scene_hash, "stored native predecessor scene hash");
      if (scene_revision <= 0) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "stored native predecessor scene revision is invalid");
      }
      expected_local = ProjectionRevisionDTO{
          RetainedBundleId(bundle), GraphRevision(graph),
          SceneRevision{SceneId(scene_id),
                        static_cast<std::uint64_t>(scene_revision),
                        ContentDigest(scene_hash)}};
    }
    core::ProjectionTransitionRowDTO row{
        core::ProjectionNativeRuntimeBindingDTO{
            core::CapabilityProviderBindingId(provider_binding_id),
            core::CapabilityProviderBindingRevision(provider_binding_revision),
            core::RuntimeUnitId(runtime_unit_id),
            core::RuntimeEpoch(runtime_epoch)},
        disposition, std::move(expected_local)};
    if (!row.valid() || !row.matches_predecessor(predecessor)) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored native transition row is invalid for its predecessor");
    }
    rows.push_back(std::move(row));
  }
  if (rows.size() != expected_count ||
      !core::CanonicalProjectionTransitionRows(rows)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored native transition rows do not match their captured count");
  }
  return rows;
}

std::vector<NativeApplyIntentState> ReadNativeApplyIntents(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query) {
  if (ScalarInteger(database,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='logical_scene_named_apply_native_intents'") == 0) {
    return {};
  }
  Statement statement(
      database, "SELECT lineage_generation,apply_id,apply_generation,"
                "projection_stage_generation,predecessor_retained_bundle_id,"
                "predecessor_graph_revision,predecessor_scene_id,"
                "predecessor_scene_revision_id,predecessor_scene_content_hash,"
                "candidate_retained_bundle_id,candidate_graph_revision,"
                "candidate_scene_id,candidate_scene_revision_id,"
                "candidate_scene_content_hash,frozen_snapshot_json,"
                "native_transition_row_count "
                "FROM logical_scene_named_apply_native_intents "
                "ORDER BY lineage_generation LIMIT 257");
  std::vector<NativeApplyIntentState> intents;
  std::size_t total_transition_rows = 0U;
  std::size_t activation_selection_count = 0U;
  while (statement.Step()) {
    if (intents.size() >= kMaximumNamedApplySceneStages) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native Apply intent history exceeds its fixed bound");
    }
    const auto lineage = statement.Integer(0);
    const auto apply_id_text = statement.Text(1);
    const auto apply_generation_value = statement.Integer(2);
    const auto stage_generation_value = statement.Integer(3);
    const auto predecessor_bundle = statement.Text(4);
    const auto predecessor_graph = statement.Text(5);
    const auto predecessor_scene_id = statement.Text(6);
    const auto predecessor_scene_revision = statement.Integer(7);
    const auto predecessor_scene_hash = statement.Text(8);
    const auto candidate_bundle = statement.Text(9);
    const auto candidate_graph = statement.Text(10);
    const auto candidate_scene_id = statement.Text(11);
    const auto candidate_scene_revision = statement.Integer(12);
    const auto candidate_scene_hash = statement.Text(13);
    const auto frozen_snapshot_json = statement.Text(14);
    const auto transition_count = statement.Integer(15);
    ValidateBoundedIdentity(apply_id_text, "stored native Apply ID");
    ValidateDigest(predecessor_bundle, "stored native predecessor bundle");
    ValidateDigest(predecessor_graph, "stored native predecessor graph");
    ValidateDigest(predecessor_scene_id, "stored native predecessor scene ID");
    ValidateDigest(predecessor_scene_hash,
                   "stored native predecessor scene hash");
    ValidateDigest(candidate_bundle, "stored native candidate bundle");
    ValidateDigest(candidate_graph, "stored native candidate graph");
    ValidateDigest(candidate_scene_id, "stored native candidate scene ID");
    ValidateDigest(candidate_scene_hash, "stored native candidate scene hash");
    if (lineage <= 0 || apply_generation_value <= 0 ||
        stage_generation_value <= 0 || predecessor_scene_revision <= 0 ||
        candidate_scene_revision <= 0 || transition_count < 0 ||
        transition_count >
            static_cast<std::int64_t>(kMaximumNativeProjectionTransitionRows) ||
        frozen_snapshot_json.empty() ||
        frozen_snapshot_json.size() > kMaximumLogicalSceneTransactionBytes) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored native Apply intent is structurally invalid");
    }
    ProjectionRevisionDTO predecessor{
        RetainedBundleId(predecessor_bundle), GraphRevision(predecessor_graph),
        SceneRevision{SceneId(predecessor_scene_id),
                      static_cast<std::uint64_t>(predecessor_scene_revision),
                      ContentDigest(predecessor_scene_hash)}};
    ProjectionRevisionDTO candidate{
        RetainedBundleId(candidate_bundle), GraphRevision(candidate_graph),
        SceneRevision{SceneId(candidate_scene_id),
                      static_cast<std::uint64_t>(candidate_scene_revision),
                      ContentDigest(candidate_scene_hash)}};
    auto snapshot =
        logical_scene_codec_internal::DecodeSnapshot(frozen_snapshot_json);
    if (!predecessor.valid() || !candidate.valid() ||
        candidate.scene_revision != predecessor.scene_revision ||
        !snapshot.has_exact_identity(
            {candidate.scene_revision, fixed_query.cell_id})) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored native Apply capture differs from its frozen scene");
    }
    NativeApplyIntentState intent{
        static_cast<std::uint64_t>(lineage),
        ApplyId(apply_id_text),
        ApplyGeneration(static_cast<std::uint64_t>(apply_generation_value)),
        ProjectionStageGeneration(
            static_cast<std::uint64_t>(stage_generation_value)),
        std::move(predecessor),
        std::move(candidate),
        std::move(snapshot),
        {},
        false};
    intent.transition_rows = ReadNativeTransitionRows(
        database, intent.apply_id, intent.apply_generation,
        intent.stage_generation, intent.predecessor,
        static_cast<std::size_t>(transition_count));
    total_transition_rows += intent.transition_rows.size();

    Statement selection(
        database,
        "SELECT candidate_retained_bundle_id,candidate_graph_revision,"
        "candidate_scene_id,candidate_scene_revision_id,"
        "candidate_scene_content_hash "
        "FROM logical_scene_named_apply_activation_selections "
        "WHERE apply_id=? AND apply_generation=? AND "
        "projection_stage_generation=? LIMIT 2");
    selection.Bind(1, intent.apply_id.value());
    selection.Bind(2, CheckedSafeCounter(intent.apply_generation.value(),
                                         "activation selection generation"));
    selection.Bind(3, CheckedSafeCounter(intent.stage_generation.value(),
                                         "activation selection stage"));
    if (selection.Step()) {
      const ProjectionRevisionDTO selected{
          RetainedBundleId(selection.Text(0)), GraphRevision(selection.Text(1)),
          SceneRevision{SceneId(selection.Text(2)),
                        static_cast<std::uint64_t>(selection.Integer(3)),
                        ContentDigest(selection.Text(4))}};
      if (selected != intent.candidate || selection.Step()) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "stored native activation selection differs from its candidate");
      }
      intent.activation_selected = true;
      ++activation_selection_count;
    }
    intents.push_back(std::move(intent));
  }
  if (ScalarInteger(
          database,
          "SELECT count(*) FROM logical_scene_named_apply_native_intents") !=
          static_cast<std::int64_t>(intents.size()) ||
      ScalarInteger(database,
                    "SELECT count(*) FROM "
                    "logical_scene_named_apply_native_transition_rows") !=
          static_cast<std::int64_t>(total_transition_rows) ||
      ScalarInteger(database,
                    "SELECT count(*) FROM "
                    "logical_scene_named_apply_activation_selections") !=
          static_cast<std::int64_t>(activation_selection_count)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native Apply intent tables have inconsistent cardinality");
  }
  return intents;
}

std::vector<NamedApplyStageState> ReadNamedApplyStages(
    sqlite3 *database, const std::optional<AcceptedState> &accepted,
    const std::optional<ActiveBundlePublicationReceiptDTO> &finalization,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query) {
  const auto native_intents = ReadNativeApplyIntents(database, fixed_query);
  Statement statement(
      database, "SELECT s.lineage_generation,s.apply_id,s.apply_generation,"
                "s.projection_stage_generation,"
                "predecessor_retained_bundle_id,predecessor_graph_revision,"
                "predecessor_scene_id,predecessor_scene_revision_id,"
                "predecessor_scene_content_hash,candidate_retained_bundle_id,"
                "candidate_graph_revision,candidate_scene_id,"
                "candidate_scene_revision_id,candidate_scene_content_hash,"
                "s.transition_row_count,s.transition_evidence_state,"
                "a.transition_evidence_state,d.transition_evidence_state,"
                "f.expected_previous_publication_generation,"
                "f.publication_generation,"
                "f.private_materialization_content_digest,"
                "f.obsolete_command_surfaces_retired_evidence_digest,"
                "f.durable_public_pointer_content_digest,"
                "f.transition_row_count,f.transition_evidence_state,"
                "f.candidate_health_row_count,f.candidate_health_state "
                "FROM logical_scene_named_apply_stages AS s "
                "LEFT JOIN logical_scene_named_apply_activations AS a USING("
                "apply_id,apply_generation,projection_stage_generation) "
                "LEFT JOIN logical_scene_named_apply_discards AS d USING("
                "apply_id,apply_generation,projection_stage_generation) "
                "LEFT JOIN logical_scene_named_apply_finalizations AS f USING("
                "apply_id,apply_generation,projection_stage_generation) "
                "ORDER BY s.lineage_generation LIMIT 257");
  std::vector<NamedApplyStageState> stages;
  std::optional<ProjectionRevisionDTO> current_projection;
  std::uint64_t expected_lineage_generation = 1U;
  std::uint64_t current_publication_generation = 0U;
  bool unresolved_transition = false;
  if (accepted.has_value() && finalization.has_value()) {
    current_projection = accepted->target;
    current_publication_generation =
        finalization->publication_generation.value();
  }
  std::int64_t activation_count = 0;
  std::int64_t discard_count = 0;
  std::int64_t named_finalization_count = 0;
  std::size_t base_stage_count = 0U;
  std::size_t next_native_intent = 0U;
  while (statement.Step()) {
    if (stages.size() >= kMaximumNamedApplySceneStages) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "named Apply stage history exceeds the fixed bound");
    }
    if (unresolved_transition) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "a later named Apply follows an unresolved transition");
    }
    const auto lineage_generation = statement.Integer(0);
    const auto apply_id = statement.Text(1);
    const auto apply_generation = statement.Integer(2);
    const auto stage_generation = statement.Integer(3);
    const auto predecessor_bundle = statement.Text(4);
    const auto predecessor_graph = statement.Text(5);
    const auto predecessor_scene_id = statement.Text(6);
    const auto predecessor_scene_revision = statement.Integer(7);
    const auto predecessor_scene_digest = statement.Text(8);
    const auto candidate_bundle = statement.Text(9);
    const auto candidate_graph = statement.Text(10);
    const auto candidate_scene_id = statement.Text(11);
    const auto candidate_scene_revision = statement.Integer(12);
    const auto candidate_scene_digest = statement.Text(13);
    const auto transition_row_count = statement.Integer(14);
    const auto evidence_state = statement.Text(15);
    const bool activated = !statement.IsNull(16);
    const bool discarded = !statement.IsNull(17);
    const bool finalized = !statement.IsNull(18);
    for (int column = 18; column <= 26; ++column) {
      if (statement.IsNull(column) == finalized) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "stored named Apply finalization has partial column presence");
      }
    }
    if (activated && statement.Text(16) != "activated_and_retired") {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored named Apply activation state is invalid");
    }
    if (discarded && statement.Text(17) != "aborted") {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored named Apply discard state is invalid");
    }
    ValidateBoundedIdentity(apply_id, "stored named Apply ID");
    ValidateDigest(predecessor_bundle, "stored predecessor bundle");
    ValidateDigest(predecessor_graph, "stored predecessor graph");
    ValidateDigest(predecessor_scene_id, "stored predecessor scene ID");
    ValidateDigest(predecessor_scene_digest, "stored predecessor scene hash");
    ValidateDigest(candidate_bundle, "stored candidate bundle");
    ValidateDigest(candidate_graph, "stored candidate graph");
    ValidateDigest(candidate_scene_id, "stored candidate scene ID");
    ValidateDigest(candidate_scene_digest, "stored candidate scene hash");
    if (!accepted.has_value() || !finalization.has_value() ||
        lineage_generation !=
            static_cast<std::int64_t>(expected_lineage_generation) ||
        apply_generation <= 0 || stage_generation <= 0 ||
        predecessor_scene_revision <= 0 || candidate_scene_revision <= 0 ||
        transition_row_count != 0 || evidence_state != "staged" ||
        (discarded && (activated || finalized)) || (finalized && !activated)) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored named Apply stage is structurally invalid");
    }
    NamedApplyStageState stage{
        expected_lineage_generation,
        ApplyId(apply_id),
        ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
        ProjectionStageGeneration(static_cast<std::uint64_t>(stage_generation)),
        ProjectionRevisionDTO{
            RetainedBundleId(predecessor_bundle),
            GraphRevision(predecessor_graph),
            SceneRevision{
                SceneId(predecessor_scene_id),
                static_cast<std::uint64_t>(predecessor_scene_revision),
                ContentDigest(predecessor_scene_digest),
            },
        },
        ProjectionRevisionDTO{
            RetainedBundleId(candidate_bundle),
            GraphRevision(candidate_graph),
            SceneRevision{
                SceneId(candidate_scene_id),
                static_cast<std::uint64_t>(candidate_scene_revision),
                ContentDigest(candidate_scene_digest),
            },
        },
        activated,
        discarded,
        std::nullopt,
        {},
        std::nullopt,
        true,
        false,
    };
    if (!stage.predecessor.valid() || !stage.candidate.valid() ||
        !current_projection.has_value() ||
        stage.predecessor.retained_bundle_id !=
            current_projection->retained_bundle_id ||
        stage.predecessor.graph_revision !=
            current_projection->graph_revision ||
        stage.predecessor.scene_revision.scene_id !=
            current_projection->scene_revision.scene_id ||
        stage.predecessor.scene_revision.revision_id <
            current_projection->scene_revision.revision_id ||
        (stage.predecessor.scene_revision.revision_id ==
             current_projection->scene_revision.revision_id &&
         stage.predecessor.scene_revision.content_hash !=
             current_projection->scene_revision.content_hash) ||
        stage.candidate.scene_revision != stage.predecessor.scene_revision) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored named Apply stage differs from the active logical lineage");
    }
    if (next_native_intent < native_intents.size() &&
        native_intents[next_native_intent].lineage_generation <
            expected_lineage_generation) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored native Apply intent has no matching completion stage");
    }
    if (next_native_intent < native_intents.size() &&
        native_intents[next_native_intent].lineage_generation ==
            expected_lineage_generation) {
      const auto &intent = native_intents[next_native_intent];
      if (intent.apply_id != stage.apply_id ||
          intent.apply_generation != stage.apply_generation ||
          intent.stage_generation != stage.stage_generation ||
          intent.predecessor != stage.predecessor ||
          intent.candidate != stage.candidate ||
          (intent.activation_selected && discarded) ||
          ((activated || finalized) && !intent.activation_selected)) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "stored native Apply intent differs from its completion stage");
      }
      stage.native_transition_rows = intent.transition_rows;
      stage.frozen_snapshot = intent.frozen_snapshot;
      stage.activation_selected = intent.activation_selected;
      ++next_native_intent;
    }
    // A public scene transaction may have advanced only the scene member
    // since the preceding structural publication. The v2 Apply row captures
    // that exact predecessor; the v4 publication ledger cross-validates the
    // interleaving after both typed histories have been read.
    current_projection = stage.predecessor;
    if (finalized) {
      const auto expected_previous_generation = statement.Integer(18);
      const auto publication_generation = statement.Integer(19);
      const auto private_materialization_digest = statement.Text(20);
      const auto retired_surfaces_digest = statement.Text(21);
      const auto public_pointer_digest = statement.Text(22);
      const auto finalized_transition_row_count = statement.Integer(23);
      const auto finalized_evidence_state = statement.Text(24);
      const auto candidate_health_row_count = statement.Integer(25);
      const auto candidate_health_state = statement.Text(26);
      ValidateDigest(private_materialization_digest,
                     "stored named Apply private materialization digest");
      ValidateDigest(retired_surfaces_digest,
                     "stored named Apply retired command-surface digest");
      ValidateDigest(public_pointer_digest,
                     "stored named Apply public-pointer digest");
      if (expected_previous_generation <= 0 || publication_generation <= 0 ||
          static_cast<std::uint64_t>(expected_previous_generation) !=
              current_publication_generation ||
          static_cast<std::uint64_t>(publication_generation) !=
              current_publication_generation + 1U ||
          finalized_transition_row_count != 0 ||
          finalized_evidence_state != "finalized" ||
          candidate_health_row_count != 0 ||
          candidate_health_state != "healthy") {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "stored named Apply finalization is structurally invalid");
      }
      ActiveBundlePublicationReceiptDTO receipt{
          ActiveBundlePublicationRequestDTO{
              NamedApplyPublicationTransactionDTO{
                  stage.apply_id,
                  stage.apply_generation,
                  stage.stage_generation,
              },
              stage.candidate,
              stage.candidate.retained_bundle_id,
              stage.candidate.graph_revision,
              ActiveBundlePublicationGeneration(
                  static_cast<std::uint64_t>(expected_previous_generation)),
              ContentDigest(private_materialization_digest),
              ContentDigest(retired_surfaces_digest),
          },
          ActiveBundlePublicationGeneration(
              static_cast<std::uint64_t>(publication_generation)),
          ContentDigest(public_pointer_digest),
      };
      if (!receipt.matches_named_apply(stage.apply_id, stage.apply_generation,
                                       stage.stage_generation,
                                       stage.candidate)) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "stored named Apply receipt is invalid");
      }
      stage.finalization_receipt = std::move(receipt);
      current_projection = stage.candidate;
      current_publication_generation =
          static_cast<std::uint64_t>(publication_generation);
      ++named_finalization_count;
    }
    activation_count += activated ? 1 : 0;
    discard_count += discarded ? 1 : 0;
    unresolved_transition = !discarded && !finalized;
    stages.push_back(std::move(stage));
    ++base_stage_count;
    ++expected_lineage_generation;
  }
  if (next_native_intent < native_intents.size()) {
    if (native_intents.size() - next_native_intent != 1U ||
        unresolved_transition || !accepted.has_value() ||
        !finalization.has_value() || !current_projection.has_value()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native Apply intent history has an invalid unresolved suffix");
    }
    const auto &intent = native_intents[next_native_intent];
    if (intent.lineage_generation != expected_lineage_generation ||
        intent.activation_selected ||
        intent.predecessor.retained_bundle_id !=
            current_projection->retained_bundle_id ||
        intent.predecessor.graph_revision !=
            current_projection->graph_revision ||
        intent.predecessor.scene_revision.scene_id !=
            current_projection->scene_revision.scene_id ||
        intent.predecessor.scene_revision.revision_id <
            current_projection->scene_revision.revision_id ||
        (intent.predecessor.scene_revision.revision_id ==
             current_projection->scene_revision.revision_id &&
         intent.predecessor.scene_revision.content_hash !=
             current_projection->scene_revision.content_hash) ||
        intent.candidate.scene_revision != intent.predecessor.scene_revision) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "unresolved native Apply intent differs from the active logical "
          "lineage");
    }
    stages.push_back(NamedApplyStageState{
        intent.lineage_generation,
        intent.apply_id,
        intent.apply_generation,
        intent.stage_generation,
        intent.predecessor,
        intent.candidate,
        false,
        false,
        std::nullopt,
        intent.transition_rows,
        intent.frozen_snapshot,
        false,
        false,
    });
    ++next_native_intent;
  }
  if (next_native_intent != native_intents.size()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native Apply intent history was not consumed exactly");
  }
  if (ScalarInteger(database,
                    "SELECT count(*) FROM logical_scene_named_apply_stages") !=
          static_cast<std::int64_t>(base_stage_count) ||
      ScalarInteger(
          database,
          "SELECT count(*) FROM logical_scene_named_apply_activations") !=
          activation_count ||
      ScalarInteger(
          database,
          "SELECT count(*) FROM logical_scene_named_apply_discards") !=
          discard_count ||
      ScalarInteger(
          database,
          "SELECT count(*) FROM logical_scene_named_apply_finalizations") !=
          named_finalization_count) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply typed phase rows do not match the stage lineage");
  }
  return stages;
}

[[nodiscard]] std::vector<core::ProjectionNativeRuntimeBindingDTO>
ReadNativeRuntimeRecoveryBindings(sqlite3 *database,
                                  const std::uint64_t recovery_generation,
                                  const std::size_t expected_count,
                                  const ProjectionRevisionDTO &target) {
  Statement statement(
      database, "SELECT binding_ordinal,provider_binding_id,"
                "provider_binding_revision,runtime_unit_id,runtime_epoch FROM "
                "logical_scene_native_runtime_recovery_bindings WHERE "
                "recovery_generation=? ORDER BY binding_ordinal LIMIT 257");
  statement.Bind(
      1, CheckedSafeCounter(recovery_generation, "native recovery generation"));
  std::vector<core::ProjectionNativeRuntimeBindingDTO> bindings;
  while (statement.Step()) {
    if (bindings.size() >= kMaximumNativeProjectionTransitionRows ||
        statement.Integer(0) != static_cast<std::int64_t>(bindings.size())) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native recovery binding order or cardinality is invalid");
    }
    const auto provider_binding_id = statement.Text(1);
    const auto provider_binding_revision = statement.Text(2);
    const auto runtime_unit_id = statement.Text(3);
    const auto runtime_epoch = statement.Text(4);
    ValidateBoundedIdentity(provider_binding_id,
                            "stored recovery provider binding ID");
    ValidateBoundedIdentity(provider_binding_revision,
                            "stored recovery provider binding revision");
    ValidateBoundedIdentity(runtime_unit_id, "stored recovery runtime unit ID");
    ValidateBoundedIdentity(runtime_epoch, "stored recovery runtime epoch");
    bindings.push_back({
        core::CapabilityProviderBindingId(provider_binding_id),
        core::CapabilityProviderBindingRevision(provider_binding_revision),
        core::RuntimeUnitId(runtime_unit_id),
        core::RuntimeEpoch(runtime_epoch),
    });
  }
  if (bindings.size() != expected_count ||
      !ActiveProjectionRuntimeSetDTO{target, bindings}.valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored native recovery bindings are not exact and canonical");
  }
  return bindings;
}

[[nodiscard]] bool SameProjectionRequirementSet(
    const NamedApplyStageState &stage,
    const std::vector<core::ProjectionNativeRuntimeBindingDTO> &bindings) {
  std::map<ProjectionProviderBindingKey, core::RuntimeUnitId> expected;
  for (const auto &row : stage.native_transition_rows) {
    if (row.disposition == core::ProjectionTransitionDisposition::kRemoved) {
      continue;
    }
    expected.emplace(
        ProjectionProviderBindingKey{
            row.native_runtime_binding.provider_binding_id,
            row.native_runtime_binding.provider_binding_revision},
        row.native_runtime_binding.runtime_unit_id);
  }
  if (expected.size() != bindings.size()) {
    return false;
  }
  for (const auto &binding : bindings) {
    const auto found = expected.find(
        {binding.provider_binding_id, binding.provider_binding_revision});
    if (found == expected.end() || found->second != binding.runtime_unit_id) {
      return false;
    }
    expected.erase(found);
  }
  return expected.empty();
}

[[nodiscard]] std::optional<std::vector<core::ProjectionHealthDTO>>
ReadNativeRuntimeRecoveryCompletion(
    sqlite3 *database, const std::uint64_t recovery_generation,
    const ProjectionRevisionDTO &target,
    const std::vector<core::ProjectionNativeRuntimeBindingDTO> &bindings) {
  Statement completion(
      database, "SELECT result_count FROM "
                "logical_scene_native_runtime_recovery_completions WHERE "
                "recovery_generation=? LIMIT 2");
  completion.Bind(
      1, CheckedSafeCounter(recovery_generation, "native recovery generation"));
  if (!completion.Step()) {
    Statement result_count(
        database,
        "SELECT count(*) FROM logical_scene_native_runtime_recovery_results "
        "WHERE recovery_generation=?");
    result_count.Bind(1, CheckedSafeCounter(recovery_generation,
                                            "native recovery generation"));
    if (!result_count.Step() || result_count.Integer(0) != 0 ||
        result_count.Step()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "incomplete native recovery carries result rows");
    }
    return std::nullopt;
  }
  const auto result_count = completion.Integer(0);
  if (completion.Step() || result_count <= 0 ||
      result_count != static_cast<std::int64_t>(bindings.size())) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native recovery completion count is invalid");
  }

  Statement results(
      database,
      "SELECT binding_ordinal,source_sequence,native_projection_generation,"
      "projection_content_digest FROM "
      "logical_scene_native_runtime_recovery_results WHERE "
      "recovery_generation=? ORDER BY binding_ordinal LIMIT 257");
  results.Bind(
      1, CheckedSafeCounter(recovery_generation, "native recovery generation"));
  std::vector<core::ProjectionHealthDTO> health;
  while (results.Step()) {
    if (health.size() >= bindings.size() ||
        results.Integer(0) != static_cast<std::int64_t>(health.size()) ||
        results.Integer(1) <= 0 || results.Integer(2) <= 0) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native recovery result order or counters are invalid");
    }
    const auto digest = results.Text(3);
    ValidateDigest(digest, "stored native recovery projection digest");
    health.push_back(core::ProjectionHealthDTO{
        bindings[health.size()],
        static_cast<std::uint64_t>(results.Integer(1)),
        ProjectionHealthState::kHealthy,
        target,
        std::nullopt,
        static_cast<std::uint64_t>(results.Integer(2)),
        ContentDigest(digest),
        std::nullopt,
    });
  }
  if (health.size() != bindings.size()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native recovery result set is incomplete");
  }
  return health;
}

[[nodiscard]] std::vector<NativeRuntimeRecoveryState>
ReadNativeRuntimeRecoveries(
    sqlite3 *database,
    const std::vector<NamedApplyStageState> &named_apply_stages) {
  const bool bounded_history = ScalarInteger(database, "PRAGMA user_version") >=
                               kLogicalSceneStoreSchemaVersion;
  const auto history = ReadBoundedHistoryState(database);
  const std::string recovery_query =
      "SELECT recovery_generation,structural_lineage_generation,"
      "target_retained_bundle_id,target_graph_revision,target_scene_id,"
      "target_scene_revision_id,target_scene_content_hash,"
      "native_binding_count," +
      std::string(bounded_history ? "recovered_transaction_sequence" : "NULL") +
      " FROM logical_scene_native_runtime_recovery_intents ORDER BY "
      "recovery_generation LIMIT 257";
  Statement statement(database, recovery_query);
  std::vector<NativeRuntimeRecoveryState> recoveries;
  std::size_t total_bindings = 0U;
  std::size_t total_results = 0U;
  std::size_t total_completions = 0U;
  std::uint64_t previous_generation = 0U;
  while (statement.Step()) {
    const auto generation_value = statement.Integer(0);
    const auto retained_limit = bounded_history
                                    ? kMaximumRetainedNativeProjectionRecoveries
                                    : kMaximumNativeProjectionRecoveries;
    if (recoveries.size() >= retained_limit || generation_value <= 0 ||
        (!bounded_history && generation_value != static_cast<std::int64_t>(
                                                     recoveries.size() + 1U)) ||
        (bounded_history &&
         static_cast<std::uint64_t>(generation_value) <= previous_generation)) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native recovery history exceeds or breaks its bounded generation "
          "order");
    }
    const auto generation = static_cast<std::uint64_t>(generation_value);
    previous_generation = generation;
    const auto lineage = statement.Integer(1);
    const auto bundle = statement.Text(2);
    const auto graph = statement.Text(3);
    const auto scene_id = statement.Text(4);
    const auto scene_revision = statement.Integer(5);
    const auto scene_hash = statement.Text(6);
    const auto binding_count = statement.Integer(7);
    const auto recovered_transaction_sequence =
        statement.IsNull(8)
            ? std::nullopt
            : std::optional<std::uint64_t>{
                  static_cast<std::uint64_t>(statement.Integer(8))};
    ValidateDigest(bundle, "stored recovery target bundle");
    ValidateDigest(graph, "stored recovery target graph");
    ValidateDigest(scene_id, "stored recovery target scene ID");
    ValidateDigest(scene_hash, "stored recovery target scene hash");
    if (lineage <= 0 || scene_revision <= 0 || binding_count <= 0 ||
        (recovered_transaction_sequence.has_value() &&
         *recovered_transaction_sequence == 0U) ||
        binding_count >
            static_cast<std::int64_t>(kMaximumNativeProjectionTransitionRows)) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored native recovery intent is structurally invalid");
    }
    const auto stage =
        std::find_if(named_apply_stages.begin(), named_apply_stages.end(),
                     [&](const NamedApplyStageState &candidate) {
                       return candidate.lineage_generation ==
                              static_cast<std::uint64_t>(lineage);
                     });
    ProjectionRevisionDTO target{
        RetainedBundleId(bundle), GraphRevision(graph),
        SceneRevision{SceneId(scene_id),
                      static_cast<std::uint64_t>(scene_revision),
                      ContentDigest(scene_hash)}};
    if (stage == named_apply_stages.end() ||
        !stage->finalization_receipt.has_value() ||
        stage->native_transition_rows.empty() || !target.valid() ||
        target.retained_bundle_id != stage->candidate.retained_bundle_id ||
        target.graph_revision != stage->candidate.graph_revision ||
        target.scene_revision.scene_id !=
            stage->candidate.scene_revision.scene_id ||
        target.scene_revision.revision_id <
            stage->candidate.scene_revision.revision_id) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native recovery target is not in its finalized structural "
          "lineage");
    }
    auto bindings = ReadNativeRuntimeRecoveryBindings(
        database, generation, static_cast<std::size_t>(binding_count), target);
    if (!SameProjectionRequirementSet(*stage, bindings)) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native recovery bindings differ from the finalized provider set");
    }
    auto completion = ReadNativeRuntimeRecoveryCompletion(database, generation,
                                                          target, bindings);
    total_bindings += bindings.size();
    total_results += completion.has_value() ? completion->size() : 0U;
    total_completions += completion.has_value() ? 1U : 0U;
    recoveries.push_back({NativeProjectionRecoveryGeneration(generation),
                          static_cast<std::uint64_t>(lineage),
                          std::move(target), std::move(bindings),
                          recovered_transaction_sequence,
                          std::move(completion)});
  }
  if (ScalarInteger(database,
                    "SELECT count(*) FROM "
                    "logical_scene_native_runtime_recovery_intents") !=
          static_cast<std::int64_t>(recoveries.size()) ||
      ScalarInteger(database,
                    "SELECT count(*) FROM "
                    "logical_scene_native_runtime_recovery_bindings") !=
          static_cast<std::int64_t>(total_bindings) ||
      ScalarInteger(database,
                    "SELECT count(*) FROM "
                    "logical_scene_native_runtime_recovery_results") !=
          static_cast<std::int64_t>(total_results) ||
      ScalarInteger(database,
                    "SELECT count(*) FROM "
                    "logical_scene_native_runtime_recovery_completions") !=
          static_cast<std::int64_t>(total_completions)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native recovery tables have inconsistent cardinality");
  }
  if (bounded_history &&
      ((!recoveries.empty() &&
        recoveries.back().recovery_generation.value() >
            history.native_recovery_generation_watermark) ||
       (recoveries.empty() &&
        history.native_recovery_generation_watermark != 0U &&
        ScalarInteger(database,
                      "SELECT count(*) FROM "
                      "logical_scene_native_runtime_recovery_intents") != 0))) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native recovery history exceeds its durable generation watermark");
  }
  return recoveries;
}

DurableState
ReadDurableState(sqlite3 *database,
                 const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query) {
  DurableState state;
  state.accepted = ReadAcceptance(database, fixed_query);
  state.rebuilt = ReadRebuild(database, state.accepted);
  state.finalization_receipt =
      ReadFinalization(database, state.accepted, state.rebuilt);
  state.named_apply_stages = ReadNamedApplyStages(
      database, state.accepted, state.finalization_receipt, fixed_query);
  if (state.accepted.has_value() && state.finalization_receipt.has_value()) {
    state.active_projection = state.accepted->target;
    state.active_scene_snapshot = state.accepted->snapshot;
    for (const auto &stage : state.named_apply_stages) {
      if (stage.finalization_receipt.has_value()) {
        state.active_projection = stage.candidate;
      }
    }
  }
  return state;
}

DurableState ReadCompleteDurableState(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query) {
  auto state = ReadDurableState(database, fixed_query);
  if (!state.finalization_receipt.has_value()) {
    state.native_runtime_recoveries =
        ReadNativeRuntimeRecoveries(database, state.named_apply_stages);
    return state;
  }
  const auto transactions = ReadSceneTransactionStoreState(database, state);
  state.active_projection = transactions.active_projection;
  state.active_scene_snapshot = transactions.active_snapshot;
  state.projection_desynchronized = transactions.projection_desynchronized;
  state.unresolved_scene_transaction = transactions.unresolved_native_attempt;
  state.latest_uncertain_transaction_sequence =
      transactions.latest_uncertain_transaction_sequence;
  state.native_runtime_recoveries =
      ReadNativeRuntimeRecoveries(database, state.named_apply_stages);
  const auto history = ReadBoundedHistoryState(database);
  for (const auto &recovery : state.native_runtime_recoveries) {
    if (!recovery.recovered_transaction_sequence.has_value()) {
      continue;
    }
    const auto sequence = *recovery.recovered_transaction_sequence;
    if (sequence >= transactions.next_transaction_sequence ||
        (recovery.completion_health.has_value() &&
         history.recovered_through_transaction_sequence < sequence) ||
        (!recovery.completion_health.has_value() &&
         (history.recovered_through_transaction_sequence >= sequence ||
          !transactions.latest_uncertain_transaction_sequence.has_value() ||
          *transactions.latest_uncertain_transaction_sequence != sequence ||
          !transactions.projection_desynchronized))) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native recovery transaction watermark does not match the exact "
          "interrupted scene transaction");
    }
  }
  if (const auto *latest = state.LatestActiveRuntimeRecovery();
      latest != nullptr &&
      (latest->target.retained_bundle_id !=
           state.active_projection->retained_bundle_id ||
       latest->target.graph_revision !=
           state.active_projection->graph_revision ||
       latest->target.scene_revision.scene_id !=
           state.active_projection->scene_revision.scene_id ||
       latest->target.scene_revision.revision_id >
           state.active_projection->scene_revision.revision_id ||
       (latest->target.scene_revision.revision_id ==
            state.active_projection->scene_revision.revision_id &&
        latest->target.scene_revision.content_hash !=
            state.active_projection->scene_revision.content_hash))) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "latest native recovery target is not an observed active scene "
        "revision");
  }
  return state;
}

bool RecoverIncompleteSceneTransaction(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query) {
  Transaction recovery(database, "BEGIN IMMEDIATE");
  const auto durable_state = ReadDurableState(database, fixed_query);
  if (!durable_state.finalization_receipt.has_value()) {
    recovery.Commit();
    return false;
  }
  const auto transactions =
      ReadSceneTransactionStoreState(database, durable_state);
  if (!transactions.unresolved_transaction_sequence.has_value()) {
    recovery.Commit();
    return false;
  }
  const auto sequence = *transactions.unresolved_transaction_sequence;
  Statement intent(
      database,
      "SELECT request_owner_epoch,request_id,request_digest,"
      "required_projection_count,proposed_retained_bundle_id,"
      "proposed_graph_revision,proposed_scene_id,"
      "proposed_scene_revision_id,proposed_scene_content_hash FROM "
      "logical_scene_transaction_intents WHERE transaction_sequence=? "
      "LIMIT 2");
  intent.Bind(1, CheckedSafeCounter(sequence, "transaction_sequence"));
  if (!intent.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "unresolved scene transaction intent is absent");
  }
  const auto request_owner_epoch = intent.Text(0);
  const RequestId request_id(intent.Text(1));
  const ContentDigest request_digest(intent.Text(2));
  const auto required_count_value = intent.Integer(3);
  const auto proposed = ReadProjectionColumns(intent, 4);
  if (intent.Step() || required_count_value < 0 || required_count_value > 256) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "unresolved scene transaction intent is invalid");
  }
  const auto required_count = static_cast<std::size_t>(required_count_value);
  const auto evidence =
      ReadProjectionEvidenceState(database, sequence, proposed);

  bool success = false;
  SceneProjectionCommitStatusDTO status =
      SceneProjectionCommitStatusDTO::kNotAttempted;
  std::optional<ErrorDTO> error;
  if (evidence.attempt_count == 0U) {
    if (required_count == 0U) {
      success = true;
      status = SceneProjectionCommitStatusDTO::kCommitted;
    } else {
      error = SceneError(
          "scene_request_recovered_before_projection",
          "scene request was durably accepted but no native projection call "
          "began before owner restart");
    }
  } else if (evidence.result_count == evidence.attempt_count &&
             evidence.attempt_count == required_count &&
             evidence.all_results_committed) {
    success = true;
    status = SceneProjectionCommitStatusDTO::kCommitted;
  } else if (evidence.result_count == evidence.attempt_count &&
             evidence.all_results_rollback_proven) {
    status = SceneProjectionCommitStatusDTO::kRejectedRollbackProven;
    error = SceneError(
        "scene_projection_rejected_rollback_proven",
        "restart verified that every invoked projection retained the "
        "predecessor state");
  } else {
    status = SceneProjectionCommitStatusDTO::kFailedProjectionUncertain;
    error =
        SceneError("scene_projection_recovery_uncertain",
                   "restart cannot prove a common native predecessor after the "
                   "interrupted scene transaction");
  }
  InsertSceneOutcome(database, request_owner_epoch, request_id, request_digest,
                     sequence, success, status, evidence.attempt_count, error);
  const auto recovered = ReadCompleteDurableState(database, fixed_query);
  if (recovered.unresolved_scene_transaction ||
      (success && (!recovered.active_projection.has_value() ||
                   *recovered.active_projection != proposed)) ||
      (!success &&
       status == SceneProjectionCommitStatusDTO::kFailedProjectionUncertain &&
       !recovered.projection_desynchronized)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "scene transaction recovery did not settle the durable owner state");
  }
  recovery.Commit();
  return true;
}

void ValidateMetadata(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  Statement statement(
      database,
      "SELECT schema_id,schema_version,schema_artifact_digest,"
      "canonical_empty_admission_digest,cell_id,"
      "canonical_empty_retained_bundle_id,canonical_empty_graph_revision,"
      "maximum_identifier_bytes,maximum_database_page_image_bytes,"
      "maximum_wal_bytes,maximum_named_apply_stages "
      "FROM logical_scene_store_metadata "
      "ORDER BY singleton LIMIT 2");
  if (!statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene metadata row is missing");
  }
  const auto schema_id = statement.Text(0);
  const auto schema_version = statement.Integer(1);
  const auto schema_digest = statement.Text(2);
  const auto stored_admission_digest = statement.Text(3);
  const auto cell_id = statement.Text(4);
  const auto retained_bundle_id = statement.Text(5);
  const auto graph_revision = statement.Text(6);
  const auto maximum_identifier_bytes = statement.Integer(7);
  const auto maximum_database_bytes = statement.Integer(8);
  const auto maximum_wal_bytes = statement.Integer(9);
  const auto maximum_named_apply_stages = statement.Integer(10);
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene metadata cardinality exceeds one");
  }
  if (schema_id != kCanonicalEmptySceneStoreSchemaId ||
      schema_version != kCanonicalEmptySceneStoreSchemaVersion ||
      schema_digest != BaseSchemaArtifactDigest() ||
      stored_admission_digest != admission_digest.value() ||
      cell_id != fixed_query.cell_id.value() ||
      retained_bundle_id !=
          fixed_query.canonical_empty_retained_bundle_id.value() ||
      graph_revision != fixed_query.canonical_empty_graph_revision.value() ||
      maximum_identifier_bytes !=
          static_cast<std::int64_t>(kMaximumSceneStoreIdentifierBytes) ||
      maximum_database_bytes != kMaximumSceneStoreDatabasePageImageBytes ||
      maximum_wal_bytes != kMaximumSceneStoreWalBytes ||
      maximum_named_apply_stages !=
          static_cast<std::int64_t>(kMaximumNamedApplySceneStages)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene metadata does not match the fixed v2 owner");
  }
}

void ValidateMechanicalDatabaseIntegrity(sqlite3 *database) {
  if (ScalarText(database, "PRAGMA quick_check(1)") != "ok") {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene SQLite quick_check failed");
  }
  Statement foreign_keys(database, "PRAGMA foreign_key_check");
  if (foreign_keys.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene SQLite foreign-key check failed");
  }
  const auto page_count = ScalarInteger(database, "PRAGMA page_count");
  const auto page_size = ScalarInteger(database, "PRAGMA page_size");
  if (page_count < 0 || page_size <= 0 ||
      page_count > kMaximumSceneStoreDatabasePageImageBytes / page_size) {
    throw CanonicalEmptySceneOwnerLimitError(
        "logical-scene database page image exceeds its fixed ceiling");
  }
}

void ValidateBaseDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
          kCanonicalEmptySceneSqliteApplicationId ||
      ScalarInteger(database, "PRAGMA user_version") !=
          kCanonicalEmptySceneStoreSchemaVersion) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene SQLite base application/schema version is invalid");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  if (ReadSchemaObjects(database) != ExpectedBaseSchemaObjects()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene base schema differs from its exact v2 artifact");
  }
  ValidateMetadata(database, fixed_query, admission_digest);
  static_cast<void>(ReadDurableState(database, fixed_query));
}

void ValidateMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database,
      "SELECT schema_id,schema_version,migration_artifact_digest,"
      "maximum_scene_transactions,maximum_transaction_bytes "
      "FROM logical_scene_store_migrations ORDER BY singleton LIMIT 2");
  if (!statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene transaction migration metadata is missing");
  }
  const auto valid =
      statement.Text(0) == kLogicalSceneTransactionStoreSchemaId &&
      statement.Integer(1) == kLogicalSceneTransactionStoreSchemaVersion &&
      statement.Text(2) == TransactionMigrationArtifactDigest() &&
      statement.Integer(3) ==
          static_cast<std::int64_t>(kMaximumLogicalSceneTransactions) &&
      statement.Integer(4) ==
          static_cast<std::int64_t>(kMaximumLogicalSceneTransactionBytes) &&
      !statement.Step();
  if (!valid) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene transaction migration metadata is invalid");
  }
}

void ValidateNativeHandoffMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database,
      "SELECT schema_id,schema_version,migration_artifact_digest,"
      "maximum_native_transition_rows,maximum_native_recoveries,"
      "maximum_frozen_snapshot_bytes "
      "FROM logical_scene_native_handoff_migrations ORDER BY singleton "
      "LIMIT 2");
  if (!statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native projection handoff migration metadata is missing");
  }
  const auto valid =
      statement.Text(0) == kNativeProjectionHandoffStoreSchemaId &&
      statement.Integer(1) == kNativeProjectionHandoffStoreSchemaVersion &&
      statement.Text(2) == NativeHandoffMigrationArtifactDigest() &&
      statement.Integer(3) ==
          static_cast<std::int64_t>(kMaximumNativeProjectionTransitionRows) &&
      statement.Integer(4) ==
          static_cast<std::int64_t>(kMaximumNativeProjectionRecoveries) &&
      statement.Integer(5) ==
          static_cast<std::int64_t>(kMaximumLogicalSceneTransactionBytes) &&
      !statement.Step();
  if (!valid) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native projection handoff migration metadata is invalid");
  }
}

void ValidateBoundedHistoryMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database,
      "SELECT schema_id,schema_version,migration_artifact_digest,"
      "maximum_resident_scene_transactions,"
      "maximum_retained_native_recoveries "
      "FROM logical_scene_bounded_history_migrations ORDER BY singleton "
      "LIMIT 2");
  if (!statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "bounded logical-scene history migration metadata is missing");
  }
  const auto valid =
      statement.Text(0) == kLogicalSceneStoreSchemaId &&
      statement.Integer(1) == kLogicalSceneStoreSchemaVersion &&
      statement.Text(2) == BoundedHistoryMigrationArtifactDigest() &&
      statement.Integer(3) ==
          static_cast<std::int64_t>(kMaximumLogicalSceneTransactions) &&
      statement.Integer(4) == static_cast<std::int64_t>(
                                  kMaximumRetainedNativeProjectionRecoveries) &&
      !statement.Step();
  if (!valid) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "bounded logical-scene history migration metadata is invalid");
  }
}

void ValidateTransactionDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
          kCanonicalEmptySceneSqliteApplicationId ||
      ScalarInteger(database, "PRAGMA user_version") !=
          kLogicalSceneTransactionStoreSchemaVersion) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene SQLite transaction schema version is invalid");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  if (ReadSchemaObjects(database) != ExpectedTransactionSchemaObjects()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene schema differs from its exact v4 artifacts");
  }
  ValidateMetadata(database, fixed_query, admission_digest);
  ValidateMigrationMetadata(database);
  const auto durable_state = ReadDurableState(database, fixed_query);
  if (durable_state.finalization_receipt.has_value()) {
    static_cast<void>(ReadSceneTransactionStoreState(database, durable_state));
  }
}

void ValidateNativeHandoffDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
          kCanonicalEmptySceneSqliteApplicationId ||
      ScalarInteger(database, "PRAGMA user_version") !=
          kNativeProjectionHandoffStoreSchemaVersion) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native projection handoff SQLite schema version is invalid");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  if (ReadSchemaObjects(database) != ExpectedNativeHandoffSchemaObjects()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene schema differs from its exact v5 artifacts");
  }
  ValidateMetadata(database, fixed_query, admission_digest);
  ValidateMigrationMetadata(database);
  ValidateNativeHandoffMigrationMetadata(database);
  const auto durable_state = ReadDurableState(database, fixed_query);
  if (durable_state.finalization_receipt.has_value()) {
    static_cast<void>(ReadSceneTransactionStoreState(database, durable_state));
  }
}

void ValidateDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
          kCanonicalEmptySceneSqliteApplicationId ||
      ScalarInteger(database, "PRAGMA user_version") !=
          kLogicalSceneStoreSchemaVersion) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene SQLite application/schema version is invalid");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  if (ReadSchemaObjects(database) != ExpectedSchemaObjects()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene schema differs from its exact v6 artifacts");
  }
  ValidateMetadata(database, fixed_query, admission_digest);
  ValidateMigrationMetadata(database);
  ValidateNativeHandoffMigrationMetadata(database);
  ValidateBoundedHistoryMigrationMetadata(database);
  const auto history = ReadBoundedHistoryState(database);
  const auto durable_state = ReadDurableState(database, fixed_query);
  if (durable_state.finalization_receipt.has_value()) {
    static_cast<void>(ReadSceneTransactionStoreState(database, durable_state));
  } else if (ScalarInteger(
                 database,
                 "SELECT count(*) FROM logical_scene_transaction_intents") !=
                 0 ||
             ScalarInteger(
                 database,
                 "SELECT count(*) FROM logical_scene_request_outcomes") != 0 ||
             ScalarInteger(
                 database,
                 "SELECT count(*) FROM logical_scene_projection_attempts") !=
                 0 ||
             ScalarInteger(
                 database,
                 "SELECT count(*) FROM logical_scene_projection_results") !=
                 0 ||
             ScalarInteger(database,
                           "SELECT count(*) FROM logical_scene_owner_events") !=
                 0 ||
             history.compaction_generation != 0U ||
             history.request_epoch_generation != 0U ||
             history.native_recovery_generation_watermark != 0U ||
             history.recovered_through_transaction_sequence != 0U) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "scene transaction history exists before bootstrap finalization");
  }
}

void InsertMetadata(sqlite3 *database,
                    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
                    const ContentDigest &admission_digest) {
  Statement statement(
      database,
      "INSERT INTO logical_scene_store_metadata("
      "singleton,schema_id,schema_version,schema_artifact_digest,"
      "canonical_empty_admission_digest,cell_id,"
      "canonical_empty_retained_bundle_id,canonical_empty_graph_revision,"
      "maximum_identifier_bytes,maximum_database_page_image_bytes,"
      "maximum_wal_bytes,maximum_named_apply_stages) "
      "VALUES(1,?,?,?,?,?,?,?,?,?,?,?)");
  statement.Bind(1, kCanonicalEmptySceneStoreSchemaId);
  statement.Bind(2, kCanonicalEmptySceneStoreSchemaVersion);
  statement.Bind(3, BaseSchemaArtifactDigest());
  statement.Bind(4, admission_digest.value());
  statement.Bind(5, fixed_query.cell_id.value());
  statement.Bind(6, fixed_query.canonical_empty_retained_bundle_id.value());
  statement.Bind(7, fixed_query.canonical_empty_graph_revision.value());
  statement.Bind(8,
                 static_cast<std::int64_t>(kMaximumSceneStoreIdentifierBytes));
  statement.Bind(9, kMaximumSceneStoreDatabasePageImageBytes);
  statement.Bind(10, kMaximumSceneStoreWalBytes);
  statement.Bind(11, static_cast<std::int64_t>(kMaximumNamedApplySceneStages));
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "metadata insert unexpectedly returned a row");
  }
}

void InsertMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database, "INSERT INTO logical_scene_store_migrations("
                "singleton,schema_id,schema_version,migration_artifact_digest,"
                "maximum_scene_transactions,maximum_transaction_bytes) "
                "VALUES(1,?,?,?,?,?)");
  statement.Bind(1, kLogicalSceneTransactionStoreSchemaId);
  statement.Bind(2, kLogicalSceneTransactionStoreSchemaVersion);
  statement.Bind(3, TransactionMigrationArtifactDigest());
  statement.Bind(4,
                 static_cast<std::int64_t>(kMaximumLogicalSceneTransactions));
  statement.Bind(
      5, static_cast<std::int64_t>(kMaximumLogicalSceneTransactionBytes));
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "migration metadata insert unexpectedly returned a row");
  }
}

void InsertNativeHandoffMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database, "INSERT INTO logical_scene_native_handoff_migrations("
                "singleton,schema_id,schema_version,migration_artifact_digest,"
                "maximum_native_transition_rows,maximum_native_recoveries,"
                "maximum_frozen_snapshot_bytes) VALUES(1,?,?,?,?,?,?)");
  statement.Bind(1, kNativeProjectionHandoffStoreSchemaId);
  statement.Bind(2, kNativeProjectionHandoffStoreSchemaVersion);
  statement.Bind(3, NativeHandoffMigrationArtifactDigest());
  statement.Bind(
      4, static_cast<std::int64_t>(kMaximumNativeProjectionTransitionRows));
  statement.Bind(5,
                 static_cast<std::int64_t>(kMaximumNativeProjectionRecoveries));
  statement.Bind(
      6, static_cast<std::int64_t>(kMaximumLogicalSceneTransactionBytes));
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native handoff migration metadata insert returned a row");
  }
}

void InsertBoundedHistoryMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database, "INSERT INTO logical_scene_bounded_history_migrations("
                "singleton,schema_id,schema_version,migration_artifact_digest,"
                "maximum_resident_scene_transactions,"
                "maximum_retained_native_recoveries) VALUES(1,?,?,?,?,?)");
  statement.Bind(1, kLogicalSceneStoreSchemaId);
  statement.Bind(2, kLogicalSceneStoreSchemaVersion);
  statement.Bind(3, BoundedHistoryMigrationArtifactDigest());
  statement.Bind(4,
                 static_cast<std::int64_t>(kMaximumLogicalSceneTransactions));
  statement.Bind(
      5, static_cast<std::int64_t>(kMaximumRetainedNativeProjectionRecoveries));
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "bounded-history migration metadata insert returned a row");
  }
}

void ConfigureConnection(sqlite3 *database) {
  if (sqlite3_db_config(database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) !=
          SQLITE_OK ||
      sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) !=
          SQLITE_OK) {
    throw CanonicalEmptySceneOwnerError(
        "cannot configure defensive logical-scene SQLite connection");
  }
  Check(database, sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds),
        "configure SQLite busy timeout");
  sqlite3_extended_result_codes(database, 1);
  sqlite3_limit(database, SQLITE_LIMIT_LENGTH, kMaximumSqliteValueBytes);
  sqlite3_limit(database, SQLITE_LIMIT_SQL_LENGTH, kMaximumSqliteValueBytes);
  sqlite3_limit(database, SQLITE_LIMIT_ATTACHED, 0);
  Exec(database, "PRAGMA foreign_keys=ON");
}

void ConfigureDurability(sqlite3 *database) {
  Exec(database, "PRAGMA page_size=4096");
  if (ScalarText(database, "PRAGMA journal_mode=WAL") != "wal") {
    throw CanonicalEmptySceneOwnerError(
        "logical-scene database did not enter WAL mode");
  }
  Exec(database, "PRAGMA synchronous=FULL");
  Exec(database, "PRAGMA wal_autocheckpoint=1000");
  Exec(database, "PRAGMA journal_size_limit=16777216");
  Statement maximum_pages(database, "PRAGMA max_page_count=16384");
  if (!maximum_pages.Step() || maximum_pages.Integer(0) > kMaximumPageCount ||
      maximum_pages.Step()) {
    throw CanonicalEmptySceneOwnerLimitError(
        "cannot enforce logical-scene database page ceiling");
  }
}

bool IsExactUninitializedDatabase(sqlite3 *database) {
  return ScalarInteger(database,
                       "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE "
                       "'sqlite_%'") == 0 &&
         ScalarInteger(database, "PRAGMA application_id") == 0 &&
         ScalarInteger(database, "PRAGMA user_version") == 0;
}

void FsyncDatabaseDirectory(const std::filesystem::path &database_path) {
  auto directory = database_path.parent_path();
  if (directory.empty()) {
    directory = ".";
  }
  const int descriptor =
      open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    throw CanonicalEmptySceneOwnerError(
        "cannot open logical-scene database directory for fsync: " +
        std::string(std::strerror(errno)));
  }
  if (fsync(descriptor) != 0) {
    const auto detail = std::string(std::strerror(errno));
    close(descriptor);
    throw CanonicalEmptySceneOwnerError(
        "cannot fsync logical-scene database directory: " + detail);
  }
  if (close(descriptor) != 0) {
    throw CanonicalEmptySceneOwnerError(
        "cannot close logical-scene database directory: " +
        std::string(std::strerror(errno)));
  }
}

void ValidateCanonicalEmptyIntent(
    const ProjectionBootstrapIntentDTO &intent,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const RetainedBundleDTO &admitted_bundle) {
  // Check cardinality before the DTO's canonical-binding loop. A retained
  // execution-host process is infrastructure, not a native scene projection;
  // the structurally empty predecessor still has zero native bindings.
  if (!intent.native_runtime_bindings.empty()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "canonical-empty owner does not implement nonzero native bindings");
  }
  if (!intent.valid()) {
    throw std::invalid_argument("projection bootstrap intent is invalid");
  }
  ValidateBoundedIdentity(intent.bootstrap_id.value(), "bootstrap_id");
  static_cast<void>(CheckedSafeCounter(intent.bootstrap_generation.value(),
                                       "bootstrap_generation"));
  const auto &bundle = intent.canonical_empty_retained_bundle;
  const auto &graph = bundle.cell_graph;
  if (intent.cell_id != fixed_query.cell_id ||
      bundle.api_version != core::kRetainedBundleApiVersion ||
      bundle.retained_bundle_id !=
          fixed_query.canonical_empty_retained_bundle_id ||
      bundle != admitted_bundle ||
      graph.api_version != "manyforge.cell_graph.v1" ||
      graph.cell_id != fixed_query.cell_id ||
      graph.graph_revision != fixed_query.canonical_empty_graph_revision ||
      graph.runtime_units.size() > 1U || !graph.model_instances.empty() ||
      !graph.controllable_components.empty() ||
      !graph.control_domains.empty() || !graph.control_bindings.empty() ||
      !graph.component_attachments.empty() ||
      !bundle.desired_control_configurations.empty() ||
      !bundle.capability_provider_bindings.empty() ||
      !bundle.compiled_products.viewer_model.has_value() ||
      !bundle.compiled_products.control_domain_products.empty() ||
      !bundle.compiled_products.symbol_role_maps.empty() ||
      bundle.compiled_products.planning_collision_model.product_id.empty() ||
      bundle.compiled_products.planning_collision_model.product_kind !=
          core::RetainedProductKind::kPlanningCollisionModel ||
      bundle.compiled_products.planning_collision_model.product_contract_id
          .empty()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "bootstrap intent does not match the admitted canonical-empty "
        "cell/bundle/graph envelope");
  }
  if (graph.runtime_units.empty() !=
      bundle.compiled_products.runtime_unit_products.empty()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "canonical-empty execution host declaration and product disagree");
  }
  if (!graph.runtime_units.empty()) {
    const auto &runtime = graph.runtime_units.front();
    const auto &products =
        bundle.compiled_products.runtime_unit_products.front();
    if (!runtime.dependency_runtime_unit_ids.empty() ||
        products.runtime_unit_id != runtime.runtime_unit_id ||
        products.products.size() != 1U ||
        products.products.front().product_kind !=
            core::RetainedProductKind::kRuntimeUnitConfiguration ||
        products.products.front().product_contract_id.empty()) {
      throw CanonicalEmptySceneOwnerConflictError(
          "canonical-empty execution host envelope is incomplete");
    }
  }
  const auto &viewer_model = *bundle.compiled_products.viewer_model;
  if (viewer_model.product_id.empty() ||
      viewer_model.product_kind != core::RetainedProductKind::kViewerModel ||
      viewer_model.product_contract_id.empty()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "bootstrap intent does not match the fixed canonical-empty viewer "
        "product envelope");
  }
  ValidateDigest(bundle.compiled_products.planning_collision_model
                     .product_contract_revision.value(),
                 "planning collision model contract revision");
  ValidateDigest(viewer_model.product_contract_revision.value(),
                 "viewer model contract revision");
}

AcceptedState
MintAcceptedState(const ProjectionBootstrapIntentDTO &intent,
                  const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query) {
  try {
    const auto scene_id = core::logical_scene_identity::MintSceneId(
        LogicalSceneBootstrapIdentityInputDTO{fixed_query.cell_id,
                                              intent.bootstrap_id,
                                              intent.bootstrap_generation});
    CollisionPolicySnapshot policy;
    policy.revision = {1U,
                       std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
    LogicalSceneSnapshotDTO snapshot{
        SceneRevision{
            scene_id,
            1U,
            ContentDigest("sha256:"
                          "0000000000000000000000000000000000000000000000000000"
                          "000000000000"),
        },
        fixed_query.cell_id,
        0U,
        0U,
        0U,
        std::move(policy),
        {},
        {},
    };
    snapshot.revision.content_hash =
        core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot);
    ProjectionRevisionDTO target{
        fixed_query.canonical_empty_retained_bundle_id,
        fixed_query.canonical_empty_graph_revision,
        snapshot.revision,
    };
    if (!snapshot.canonical_empty_for(
            {target.scene_revision, fixed_query.cell_id}) ||
        !target.valid()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "qualified codec did not produce an exact canonical-empty target");
    }
    return {
        intent.bootstrap_id,
        intent.bootstrap_generation,
        std::move(target),
        std::move(snapshot),
    };
  } catch (const CanonicalEmptySceneOwnerError &) {
    throw;
  } catch (const std::exception &error) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot mint canonical-empty logical-scene identity: " +
        std::string(error.what()));
  }
}

void InsertAcceptance(sqlite3 *database, const AcceptedState &accepted) {
  Statement statement(
      database,
      "INSERT INTO canonical_empty_scene_acceptance("
      "singleton,bootstrap_id,bootstrap_generation,cell_id,"
      "retained_bundle_id,graph_revision,scene_id,scene_revision_id,"
      "scene_content_hash,transaction_sequence,topology_epoch,policy_epoch,"
      "configured_policy_id,configured_policy_revision_id,"
      "configured_policy_content_hash,object_count,carried_attachment_count,"
      "native_runtime_binding_count) "
      "VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  statement.Bind(1, accepted.bootstrap_id.value());
  statement.Bind(2, CheckedSafeCounter(accepted.bootstrap_generation.value(),
                                       "bootstrap_generation"));
  statement.Bind(3, accepted.snapshot.cell_id.value());
  statement.Bind(4, accepted.target.retained_bundle_id.value());
  statement.Bind(5, accepted.target.graph_revision.value());
  statement.Bind(6, accepted.target.scene_revision.scene_id.value());
  statement.Bind(
      7, static_cast<std::int64_t>(accepted.target.scene_revision.revision_id));
  statement.Bind(8, accepted.target.scene_revision.content_hash.value());
  statement.Bind(
      9, static_cast<std::int64_t>(accepted.snapshot.transaction_sequence));
  statement.Bind(10,
                 static_cast<std::int64_t>(accepted.snapshot.topology_epoch));
  statement.Bind(11, static_cast<std::int64_t>(accepted.snapshot.policy_epoch));
  statement.Bind(12, accepted.snapshot.configured_collision_policy.policy_id);
  statement.Bind(
      13,
      static_cast<std::int64_t>(
          accepted.snapshot.configured_collision_policy.revision.revision_id));
  statement.Bind(
      14, accepted.snapshot.configured_collision_policy.revision.content_hash);
  statement.Bind(15, 0);
  statement.Bind(16, 0);
  statement.Bind(17, 0);
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "acceptance insert unexpectedly returned a row");
  }
}

void InsertRebuild(sqlite3 *database, const AcceptedState &accepted) {
  Statement statement(
      database, "INSERT INTO canonical_empty_scene_rebuild("
                "singleton,bootstrap_id,bootstrap_generation,"
                "native_rebuild_result_count,aggregate_health_state,"
                "aggregate_native_health_count) VALUES(1,?,?,0,'healthy',0)");
  statement.Bind(1, accepted.bootstrap_id.value());
  statement.Bind(2, CheckedSafeCounter(accepted.bootstrap_generation.value(),
                                       "bootstrap_generation"));
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "rebuild insert unexpectedly returned a row");
  }
}

void ValidateFinalizeRequest(const ProjectionBootstrapFinalizeDTO &request,
                             const AcceptedState &accepted) {
  if (!request.valid() || request.bootstrap_id != accepted.bootstrap_id ||
      request.bootstrap_generation != accepted.bootstrap_generation ||
      request.canonical_empty_target != accepted.target ||
      request.publication_receipt.publication_generation !=
          ActiveBundlePublicationGeneration(1U) ||
      request.publication_receipt.request
          .expected_previous_publication_generation.has_value() ||
      !request.publication_receipt.matches_bootstrap(
          accepted.bootstrap_id, accepted.bootstrap_generation,
          accepted.target)) {
    throw CanonicalEmptySceneOwnerConflictError(
        "Finalize does not carry the exact generation-one canonical-empty "
        "publication receipt");
  }
  ValidateDigest(request.publication_receipt.request
                     .private_materialization_content_digest.value(),
                 "private materialization content digest");
  ValidateDigest(
      request.publication_receipt.request
          .obsolete_managed_command_surfaces_retired_evidence_digest.value(),
      "retired command-surfaces evidence digest");
  ValidateDigest(
      request.publication_receipt.durable_public_pointer_content_digest.value(),
      "durable public-pointer content digest");
}

void InsertFinalization(sqlite3 *database,
                        const ProjectionBootstrapFinalizeDTO &finalize) {
  const auto *transaction = std::get_if<BootstrapPublicationTransactionDTO>(
      &finalize.publication_receipt.request.transaction_ref);
  if (transaction == nullptr) {
    throw CanonicalEmptySceneOwnerConflictError(
        "canonical-empty Finalize receipt is not bootstrap-tagged");
  }
  const auto &receipt = finalize.publication_receipt;
  const auto &publication_request = receipt.request;
  const auto &target = publication_request.candidate_projection_revision;
  Statement statement(
      database,
      "INSERT INTO canonical_empty_scene_finalization("
      "singleton,bootstrap_id,bootstrap_generation,publication_generation,"
      "publication_bootstrap_id,publication_bootstrap_generation,"
      "candidate_retained_bundle_id,candidate_graph_revision,"
      "candidate_scene_id,candidate_scene_revision_id,"
      "candidate_scene_content_hash,request_candidate_retained_bundle_id,"
      "request_candidate_graph_revision,"
      "private_materialization_content_digest,"
      "retired_command_surfaces_evidence_digest,"
      "durable_public_pointer_content_digest) "
      "VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  statement.Bind(1, finalize.bootstrap_id.value());
  statement.Bind(2, CheckedSafeCounter(finalize.bootstrap_generation.value(),
                                       "bootstrap_generation"));
  statement.Bind(3, CheckedSafeCounter(receipt.publication_generation.value(),
                                       "publication_generation"));
  statement.Bind(4, transaction->bootstrap_id.value());
  statement.Bind(5,
                 CheckedSafeCounter(transaction->bootstrap_generation.value(),
                                    "publication bootstrap_generation"));
  statement.Bind(6, target.retained_bundle_id.value());
  statement.Bind(7, target.graph_revision.value());
  statement.Bind(8, target.scene_revision.scene_id.value());
  statement.Bind(9,
                 static_cast<std::int64_t>(target.scene_revision.revision_id));
  statement.Bind(10, target.scene_revision.content_hash.value());
  statement.Bind(11, publication_request.candidate_retained_bundle_id.value());
  statement.Bind(12, publication_request.candidate_graph_revision.value());
  statement.Bind(
      13, publication_request.private_materialization_content_digest.value());
  statement.Bind(
      14,
      publication_request
          .obsolete_managed_command_surfaces_retired_evidence_digest.value());
  statement.Bind(15, receipt.durable_public_pointer_content_digest.value());
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "finalization insert unexpectedly returned a row");
  }
}

ProjectionAggregateHealthDTO ZeroNativeHealth(const AcceptedState &accepted) {
  return {
      BootstrapProjectionSetSourceDTO{
          accepted.bootstrap_id,
          accepted.bootstrap_generation,
          accepted.target,
      },
      ProjectionHealthState::kHealthy,
      {},
      std::nullopt,
  };
}

ProjectionBootstrapEvidenceDTO
ZeroNativeEvidence(const AcceptedState &accepted) {
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.target,
      {},
      ZeroNativeHealth(accepted),
  };
}

ProjectionBootstrapAcceptedDTO AcceptedResult(const AcceptedState &accepted) {
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.snapshot.cell_id,
      accepted.target,
      LogicalSceneSnapshotRoot(
          std::make_shared<const LogicalSceneSnapshotDTO>(accepted.snapshot)),
  };
}

CanonicalEmptySceneAccessStatus AccessStatus(const DurableState &state) {
  const bool has_active = state.active_projection.has_value();
  const bool transition_open = state.OpenNamedApply() != nullptr;
  const auto *latest_recovery = state.LatestActiveRuntimeRecovery();
  const bool recovery_complete = latest_recovery == nullptr ||
                                 latest_recovery->completion_health.has_value();
  const bool projection_available = recovery_complete &&
                                    !state.projection_desynchronized &&
                                    !state.unresolved_scene_transaction;
  return {
      state.phase(),
      has_active && !transition_open && projection_available,
      has_active && !transition_open && projection_available,
      state.active_projection,
  };
}

ProjectionBootstrapStatusDTO
StatusResult(const DurableState &state,
             const ProjectionBootstrapStatusQueryDTO &query) {
  if (!state.accepted.has_value() ||
      query.bootstrap_id != state.accepted->bootstrap_id ||
      query.bootstrap_generation != state.accepted->bootstrap_generation) {
    return {
        query.bootstrap_id,
        query.bootstrap_generation,
        ProjectionBootstrapStatusState::kAbsent,
        std::nullopt,
        {},
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
  }
  const auto &accepted = *state.accepted;
  if (state.finalization_receipt.has_value()) {
    return {
        accepted.bootstrap_id,
        accepted.bootstrap_generation,
        ProjectionBootstrapStatusState::kFinalized,
        accepted.target,
        {},
        ZeroNativeEvidence(accepted),
        state.finalization_receipt,
        ZeroNativeHealth(accepted),
        std::nullopt,
    };
  }
  if (state.rebuilt) {
    return {
        accepted.bootstrap_id,
        accepted.bootstrap_generation,
        ProjectionBootstrapStatusState::kReadyToPublish,
        accepted.target,
        {},
        ZeroNativeEvidence(accepted),
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
  }
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      ProjectionBootstrapStatusState::kAccepted,
      accepted.target,
      {},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
  };
}

ProjectionTransitionEvidenceDTO ZeroNativeTransitionEvidence(
    const NamedApplyStageState &stage,
    const ProjectionTransitionEvidenceState evidence_state,
    std::optional<ErrorDTO> error = std::nullopt) {
  ProjectionTransitionEvidenceDTO evidence{
      stage.apply_id,
      stage.apply_generation,
      stage.stage_generation,
      stage.predecessor,
      stage.candidate,
      evidence_state,
      {},
      std::move(error),
  };
  if (!evidence.phase_consistent(evidence_state)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent zero-native named Apply transition evidence");
  }
  return evidence;
}

ProjectionTransitionEvidenceDTO NativeTransitionEvidence(
    const NamedApplyStageState &stage,
    const ProjectionTransitionEvidenceState evidence_state,
    std::vector<core::ProjectionTransitionRowEvidenceDTO> rows,
    std::optional<ErrorDTO> error = std::nullopt) {
  ProjectionTransitionEvidenceDTO evidence{
      stage.apply_id,    stage.apply_generation, stage.stage_generation,
      stage.predecessor, stage.candidate,        evidence_state,
      std::move(rows),   std::move(error),
  };
  if (!evidence.phase_consistent(evidence_state) ||
      evidence.transition_rows.size() != stage.native_transition_rows.size()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent native named Apply transition evidence");
  }
  for (std::size_t index = 0U; index < evidence.transition_rows.size();
       ++index) {
    if (evidence.transition_rows[index].transition_row !=
        stage.native_transition_rows[index]) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native named Apply evidence differs from its durable rows");
    }
  }
  return evidence;
}

core::ProjectionTransitionRowEvidenceDTO
UncertainTransitionRowEvidence(const core::ProjectionTransitionRowDTO &row,
                               core::ProjectionHealthDTO health,
                               ErrorDTO error) {
  health.state = ProjectionHealthState::kUnavailable;
  health.error = error;
  core::ProjectionTransitionRowEvidenceDTO evidence{
      row,
      core::ProjectionTransitionRowEvidenceState::kUncertain,
      std::nullopt,
      health.active_projection_revision,
      std::move(health),
      std::move(error),
  };
  if (!evidence.valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent uncertain native transition row");
  }
  return evidence;
}

core::NativeProjectionStatusDTO
ReadNativeStageStatus(const NamedApplyStageState &stage,
                      const NativeTransitionParticipant &participant) {
  const core::NativeProjectionStatusQueryDTO query{
      stage.apply_id,
      stage.apply_generation,
      stage.stage_generation,
      participant.transition_row,
  };
  auto status = participant.projection->ProjectionStageStatus(query);
  const auto &echo = status.query;
  if (!status.presence_valid() || echo.apply_id != query.apply_id ||
      echo.apply_generation != query.apply_generation ||
      echo.projection_stage_generation != query.projection_stage_generation ||
      echo.transition_row != query.transition_row) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native projection returned malformed or foreign stage status");
  }
  return status;
}

core::NativeProjectionRetireStatusDTO
ReadNativeRetireStatus(const NamedApplyStageState &stage,
                       const NativeTransitionParticipant &participant) {
  const core::NativeProjectionRetireDTO request{
      stage.apply_id,         stage.apply_generation,
      stage.stage_generation, participant.transition_row,
      stage.predecessor,      stage.candidate,
  };
  const core::NativeProjectionRetireStatusQueryDTO query{request};
  auto status = participant.projection->ProjectionRetireStatus(query);
  const auto &echo = status.request;
  if (!status.presence_valid() || echo.apply_id != request.apply_id ||
      echo.apply_generation != request.apply_generation ||
      echo.projection_stage_generation != request.projection_stage_generation ||
      echo.transition_row != request.transition_row ||
      echo.expected_predecessor_projection_revision !=
          request.expected_predecessor_projection_revision ||
      echo.candidate_projection_revision !=
          request.candidate_projection_revision) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native projection returned malformed or foreign retirement status");
  }
  return status;
}

core::ProjectionTransitionRowEvidenceDTO
StagedRowEvidence(const NamedApplyStageState &stage,
                  const NativeTransitionParticipant &participant,
                  const core::NativeProjectionStatusDTO &status) {
  const core::ProjectionStagedCandidateDTO expected_stage{
      stage.apply_id, stage.apply_generation, stage.stage_generation,
      stage.candidate};
  if (status.state != core::NativeProjectionStatusState::kStaged ||
      status.staged_or_active_candidate_revision !=
          std::optional<ProjectionRevisionDTO>{stage.candidate} ||
      status.observed_local_active !=
          participant.transition_row.expected_local_active ||
      status.resulting_health.staged_candidate !=
          std::optional<core::ProjectionStagedCandidateDTO>{expected_stage}) {
    throw CanonicalEmptySceneOwnerConflictError(
        "native projection is not exactly staged without active effect");
  }
  return {
      participant.transition_row,
      core::ProjectionTransitionRowEvidenceState::kStagedNoActiveEffect,
      stage.candidate,
      status.observed_local_active,
      status.resulting_health,
      std::nullopt,
  };
}

core::ProjectionTransitionRowEvidenceDTO PredecessorUnchangedRowEvidence(
    const NamedApplyStageState &stage,
    const NativeTransitionParticipant &participant,
    const core::NativeProjectionRetireStatusDTO &status) {
  if (status.state != core::NativeProjectionRetireStatusState::kPending ||
      status.observed_local_active !=
          participant.transition_row.expected_local_active ||
      !status.resulting_health.healthy_for_target(stage.predecessor)) {
    throw CanonicalEmptySceneOwnerConflictError(
        "removed native projection did not remain at its exact predecessor");
  }
  return {
      participant.transition_row,
      core::ProjectionTransitionRowEvidenceState::kPredecessorUnchanged,
      std::nullopt,
      status.observed_local_active,
      status.resulting_health,
      std::nullopt,
  };
}

core::ProjectionTransitionRowEvidenceDTO
CandidateActiveRowEvidence(const NamedApplyStageState &stage,
                           const NativeTransitionParticipant &participant,
                           const core::NativeProjectionStatusDTO &status) {
  if (status.state != core::NativeProjectionStatusState::kActivated ||
      status.staged_or_active_candidate_revision !=
          std::optional<ProjectionRevisionDTO>{stage.candidate} ||
      status.observed_local_active !=
          std::optional<ProjectionRevisionDTO>{stage.candidate} ||
      !status.resulting_health.healthy_for_target(stage.candidate)) {
    throw CanonicalEmptySceneOwnerConflictError(
        "candidate native projection is not exactly active");
  }
  return {
      participant.transition_row,
      core::ProjectionTransitionRowEvidenceState::kCandidateActive,
      stage.candidate,
      status.observed_local_active,
      status.resulting_health,
      std::nullopt,
  };
}

core::ProjectionTransitionRowEvidenceDTO
RemovedRetiredRowEvidence(const NativeTransitionParticipant &participant,
                          const core::NativeProjectionRetireStatusDTO &status) {
  if ((status.state != core::NativeProjectionRetireStatusState::kRetired &&
       status.state !=
           core::NativeProjectionRetireStatusState::kAlreadyAbsent) ||
      status.observed_local_active.has_value() ||
      status.resulting_health.active_projection_revision.has_value() ||
      status.resulting_health.staged_candidate.has_value() ||
      status.resulting_health.error.has_value()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "removed native projection is not positively retired");
  }
  return {
      participant.transition_row,
      core::ProjectionTransitionRowEvidenceState::kRemovedRetired,
      std::nullopt,
      std::nullopt,
      status.resulting_health,
      std::nullopt,
  };
}

core::ProjectionTransitionRowEvidenceDTO
CandidateDiscardedRowEvidence(const NamedApplyStageState &stage,
                              const NativeTransitionParticipant &participant,
                              const core::NativeProjectionStatusDTO &status) {
  if ((status.state != core::NativeProjectionStatusState::kAbsent &&
       status.state != core::NativeProjectionStatusState::kDiscarded) ||
      status.staged_or_active_candidate_revision.has_value() ||
      status.observed_local_active !=
          participant.transition_row.expected_local_active ||
      status.resulting_health.staged_candidate.has_value() ||
      (participant.transition_row.expected_local_active.has_value()
           ? !status.resulting_health.healthy_for_target(stage.predecessor)
           : status.resulting_health.state !=
                 ProjectionHealthState::kUninitialized)) {
    throw CanonicalEmptySceneOwnerConflictError(
        "candidate native stage is not positively absent");
  }
  return {
      participant.transition_row,
      core::ProjectionTransitionRowEvidenceState::kCandidateStageDiscarded,
      std::nullopt,
      status.observed_local_active,
      status.resulting_health,
      std::nullopt,
  };
}

ProjectionAggregateHealthDTO CandidateAggregateHealth(
    const NamedApplyStageState &stage,
    const std::vector<core::ProjectionTransitionRowEvidenceDTO> &rows) {
  std::vector<core::ProjectionHealthDTO> candidate_health;
  for (const auto &row : rows) {
    if (row.transition_row.disposition !=
        core::ProjectionTransitionDisposition::kRemoved) {
      candidate_health.push_back(row.resulting_health);
    }
  }
  ProjectionAggregateHealthDTO aggregate{
      NamedApplyProjectionSetSourceDTO{stage.apply_id, stage.apply_generation,
                                       stage.stage_generation, stage.candidate},
      ProjectionHealthState::kHealthy,
      std::move(candidate_health),
      std::nullopt,
  };
  if (!aggregate.matches_named_apply(stage.apply_id, stage.apply_generation,
                                     stage.stage_generation, stage.candidate) ||
      !aggregate.healthy_for_target(stage.candidate)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent complete candidate native health");
  }
  return aggregate;
}

ProjectionAggregateHealthDTO
UnavailableCandidateAggregateHealth(const NamedApplyStageState &stage,
                                    const ErrorDTO &error) {
  std::vector<core::ProjectionHealthDTO> candidate_health;
  for (const auto &row : stage.native_transition_rows) {
    if (row.disposition == core::ProjectionTransitionDisposition::kRemoved) {
      continue;
    }
    candidate_health.push_back(core::ProjectionHealthDTO{
        row.native_runtime_binding,
        1U,
        ProjectionHealthState::kUnavailable,
        row.expected_local_active,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        error,
    });
  }
  ProjectionAggregateHealthDTO aggregate{
      NamedApplyProjectionSetSourceDTO{stage.apply_id, stage.apply_generation,
                                       stage.stage_generation, stage.candidate},
      ProjectionHealthState::kUnavailable,
      std::move(candidate_health),
      error,
  };
  if (!aggregate.matches_named_apply(stage.apply_id, stage.apply_generation,
                                     stage.stage_generation, stage.candidate)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent unavailable candidate native health");
  }
  return aggregate;
}

ProjectionTransitionEvidenceDTO ObserveNativeTransitionEvidence(
    const NamedApplyStageState &stage,
    const std::vector<NativeTransitionParticipant> &participants,
    const ProjectionTransitionEvidenceState evidence_state) {
  if (evidence_state == ProjectionTransitionEvidenceState::kUncertain) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "closed native transition observation cannot manufacture uncertainty");
  }
  std::vector<core::ProjectionTransitionRowEvidenceDTO> rows;
  rows.reserve(participants.size());
  for (const auto &participant : participants) {
    const bool removed = participant.transition_row.disposition ==
                         core::ProjectionTransitionDisposition::kRemoved;
    switch (evidence_state) {
    case ProjectionTransitionEvidenceState::kStaged:
      rows.push_back(removed ? PredecessorUnchangedRowEvidence(
                                   stage, participant,
                                   ReadNativeRetireStatus(stage, participant))
                             : StagedRowEvidence(
                                   stage, participant,
                                   ReadNativeStageStatus(stage, participant)));
      break;
    case ProjectionTransitionEvidenceState::kAborted:
      rows.push_back(removed ? PredecessorUnchangedRowEvidence(
                                   stage, participant,
                                   ReadNativeRetireStatus(stage, participant))
                             : CandidateDiscardedRowEvidence(
                                   stage, participant,
                                   ReadNativeStageStatus(stage, participant)));
      break;
    case ProjectionTransitionEvidenceState::kActivatedAndRetired:
    case ProjectionTransitionEvidenceState::kFinalized:
      rows.push_back(
          removed ? RemovedRetiredRowEvidence(
                        participant, ReadNativeRetireStatus(stage, participant))
                  : CandidateActiveRowEvidence(
                        stage, participant,
                        ReadNativeStageStatus(stage, participant)));
      break;
    case ProjectionTransitionEvidenceState::kUncertain:
      throw CanonicalEmptySceneOwnerIntegrityError(
          "unreachable native transition observation state");
    }
  }
  return NativeTransitionEvidence(stage, evidence_state, std::move(rows));
}

ProjectionTransitionEvidenceDTO UncertainNativeTransitionEvidence(
    const NamedApplyStageState &stage,
    const std::vector<NativeTransitionParticipant> &participants,
    const ErrorDTO &error) {
  std::vector<core::ProjectionTransitionRowEvidenceDTO> rows;
  rows.reserve(participants.size());
  for (const auto &participant : participants) {
    rows.push_back(UncertainTransitionRowEvidence(
        participant.transition_row, participant.observed_health, error));
  }
  return NativeTransitionEvidence(stage,
                                  ProjectionTransitionEvidenceState::kUncertain,
                                  std::move(rows), error);
}

ProjectionTransitionEvidenceDTO
UnavailableNativeTransitionEvidence(const NamedApplyStageState &stage,
                                    const ErrorDTO &error) {
  std::vector<core::ProjectionTransitionRowEvidenceDTO> rows;
  rows.reserve(stage.native_transition_rows.size());
  for (const auto &row : stage.native_transition_rows) {
    core::ProjectionHealthDTO unavailable{
        row.native_runtime_binding,
        1U,
        ProjectionHealthState::kUnavailable,
        row.expected_local_active,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        error,
    };
    rows.push_back(
        UncertainTransitionRowEvidence(row, std::move(unavailable), error));
  }
  return NativeTransitionEvidence(stage,
                                  ProjectionTransitionEvidenceState::kUncertain,
                                  std::move(rows), error);
}

ProjectionTransitionEvidenceDTO
ZeroNativeStageEvidence(const NamedApplyStageState &stage) {
  return ZeroNativeTransitionEvidence(
      stage, ProjectionTransitionEvidenceState::kStaged);
}

ProjectionAggregateHealthDTO ZeroNativeNamedApplyHealth(
    const NamedApplyStageState &stage,
    const ProjectionHealthState state = ProjectionHealthState::kHealthy,
    std::optional<ErrorDTO> error = std::nullopt) {
  ProjectionAggregateHealthDTO health{
      NamedApplyProjectionSetSourceDTO{
          stage.apply_id,
          stage.apply_generation,
          stage.stage_generation,
          stage.candidate,
      },
      state,
      {},
      std::move(error),
  };
  if (!health.matches_named_apply(stage.apply_id, stage.apply_generation,
                                  stage.stage_generation, stage.candidate)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent zero-native named Apply health");
  }
  return health;
}

ProjectionApplyStageResultDTO StagedResult(const NamedApplyStageState &stage) {
  ProjectionApplyStageResultDTO result{
      stage.apply_id,
      stage.apply_generation,
      stage.predecessor,
      stage.candidate,
      stage.stage_generation,
      ZeroNativeStageEvidence(stage),
      ProjectionApplyStageResultState::kStaged,
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent zero-native named Apply staged result");
  }
  return result;
}

ProjectionApplyStageResultDTO
StagedResult(const NamedApplyStageState &stage,
             ProjectionTransitionEvidenceDTO evidence) {
  ProjectionApplyStageResultDTO result{
      stage.apply_id,
      stage.apply_generation,
      stage.predecessor,
      stage.candidate,
      stage.stage_generation,
      std::move(evidence),
      ProjectionApplyStageResultState::kStaged,
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent native named Apply staged result");
  }
  return result;
}

ProjectionApplyStageResultDTO
CapturedStageFailureResult(const NamedApplyStageState &stage,
                           const ProjectionApplyStageResultState state,
                           ProjectionTransitionEvidenceDTO evidence,
                           ErrorDTO error) {
  if (state == ProjectionApplyStageResultState::kStaged ||
      (state == ProjectionApplyStageResultState::kRejected &&
       evidence.state != ProjectionTransitionEvidenceState::kAborted) ||
      (state == ProjectionApplyStageResultState::kFailedProjectionUncertain &&
       evidence.state != ProjectionTransitionEvidenceState::kUncertain)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "captured stage failure helper received contradictory state");
  }
  ProjectionApplyStageResultDTO result{
      stage.apply_id,  stage.apply_generation, stage.predecessor,
      stage.candidate, stage.stage_generation, std::move(evidence),
      state,           std::move(error),
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent captured named Apply stage failure");
  }
  return result;
}

ProjectionApplyActivationResultDTO
ActivatedResult(const NamedApplyStageState &stage) {
  ProjectionApplyActivationResultDTO result{
      stage.apply_id,
      stage.apply_generation,
      stage.predecessor,
      stage.stage_generation,
      stage.candidate,
      ProjectionApplyActivationResultState::kActivated,
      stage.candidate,
      ZeroNativeTransitionEvidence(
          stage, ProjectionTransitionEvidenceState::kActivatedAndRetired),
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent zero-native named Apply activation");
  }
  return result;
}

ProjectionApplyActivationResultDTO
ActivatedResult(const NamedApplyStageState &stage,
                ProjectionTransitionEvidenceDTO evidence) {
  ProjectionApplyActivationResultDTO result{
      stage.apply_id,    stage.apply_generation,
      stage.predecessor, stage.stage_generation,
      stage.candidate,   ProjectionApplyActivationResultState::kActivated,
      stage.candidate,   std::move(evidence),
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent native named Apply activation");
  }
  return result;
}

ProjectionApplyDiscardResultDTO
DiscardedResult(const NamedApplyStageState &stage,
                const ProjectionApplyDiscardResultState state) {
  ProjectionApplyDiscardResultDTO result{
      stage.apply_id,
      stage.apply_generation,
      stage.stage_generation,
      stage.candidate,
      stage.predecessor,
      state,
      ZeroNativeTransitionEvidence(stage,
                                   ProjectionTransitionEvidenceState::kAborted),
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent zero-native named Apply discard");
  }
  return result;
}

ProjectionApplyDiscardResultDTO
DiscardedResult(const NamedApplyStageState &stage,
                const ProjectionApplyDiscardResultState state,
                ProjectionTransitionEvidenceDTO evidence) {
  ProjectionApplyDiscardResultDTO result{
      stage.apply_id,      stage.apply_generation, stage.stage_generation,
      stage.candidate,     stage.predecessor,      state,
      std::move(evidence), std::nullopt,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent native named Apply discard");
  }
  return result;
}

ProjectionApplyFinalizeResultDTO
FinalizedResult(const NamedApplyStageState &stage) {
  if (!stage.finalization_receipt.has_value()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot form named Apply Finalize without a durable receipt");
  }
  ProjectionApplyFinalizeResultDTO result{
      stage.apply_id,
      stage.apply_generation,
      stage.stage_generation,
      stage.candidate,
      *stage.finalization_receipt,
      ProjectionApplyFinalizeResultState::kFinalized,
      stage.candidate,
      ZeroNativeTransitionEvidence(
          stage, ProjectionTransitionEvidenceState::kFinalized),
      ZeroNativeNamedApplyHealth(stage),
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent zero-native named Apply Finalize");
  }
  return result;
}

ProjectionApplyFinalizeResultDTO
FinalizedResult(const NamedApplyStageState &stage,
                ProjectionTransitionEvidenceDTO evidence,
                ProjectionAggregateHealthDTO candidate_health) {
  if (!stage.finalization_receipt.has_value()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot form native named Apply Finalize without a durable receipt");
  }
  ProjectionApplyFinalizeResultDTO result{
      stage.apply_id,
      stage.apply_generation,
      stage.stage_generation,
      stage.candidate,
      *stage.finalization_receipt,
      ProjectionApplyFinalizeResultState::kFinalized,
      stage.candidate,
      std::move(evidence),
      std::move(candidate_health),
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent native named Apply Finalize");
  }
  return result;
}

NamedApplyStageState
StageIdentityFrom(const ProjectionApplyActivationDTO &request) {
  return {
      0U,
      request.apply_id,
      request.apply_generation,
      request.projection_stage_generation,
      request.expected_active_projection_revision,
      request.candidate_projection_revision,
      false,
      false,
      std::nullopt,
      {},
      std::nullopt,
      true,
      false,
  };
}

NamedApplyStageState
StageIdentityFrom(const ProjectionApplyDiscardDTO &request) {
  return {
      0U,
      request.apply_id,
      request.apply_generation,
      request.projection_stage_generation,
      request.expected_predecessor_projection_revision,
      request.candidate_projection_revision,
      false,
      false,
      std::nullopt,
      {},
      std::nullopt,
      true,
      false,
  };
}

ProjectionApplyActivationResultDTO
FailedActivationResult(const ProjectionApplyActivationDTO &request,
                       const ProjectionApplyActivationResultState state,
                       std::string code, std::string message) {
  if (state == ProjectionApplyActivationResultState::kActivated) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "failed activation helper received success state");
  }
  const ErrorDTO error{std::move(code), std::move(message), {}};
  const auto identity = StageIdentityFrom(request);
  const auto evidence_state =
      state == ProjectionApplyActivationResultState::kStale
          ? ProjectionTransitionEvidenceState::kAborted
          : ProjectionTransitionEvidenceState::kUncertain;
  ProjectionApplyActivationResultDTO result{
      request.apply_id,
      request.apply_generation,
      request.expected_active_projection_revision,
      request.projection_stage_generation,
      request.candidate_projection_revision,
      state,
      std::nullopt,
      ZeroNativeTransitionEvidence(
          identity, evidence_state,
          evidence_state == ProjectionTransitionEvidenceState::kUncertain
              ? std::optional<ErrorDTO>(error)
              : std::nullopt),
      error,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent failed named Apply activation");
  }
  return result;
}

ProjectionApplyActivationResultDTO
FailedActivationResult(const NamedApplyStageState &stage,
                       const ProjectionApplyActivationResultState state,
                       ProjectionTransitionEvidenceDTO evidence,
                       std::string code, std::string message) {
  if (state == ProjectionApplyActivationResultState::kActivated) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "failed native activation helper received success state");
  }
  ErrorDTO error{std::move(code), std::move(message), {}};
  ProjectionApplyActivationResultDTO result{
      stage.apply_id,         stage.apply_generation, stage.predecessor,
      stage.stage_generation, stage.candidate,        state,
      std::nullopt,           std::move(evidence),    std::move(error),
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent failed native named Apply activation");
  }
  return result;
}

ProjectionApplyDiscardResultDTO
FailedDiscardResult(const ProjectionApplyDiscardDTO &request, std::string code,
                    std::string message) {
  const ErrorDTO error{std::move(code), std::move(message), {}};
  const auto identity = StageIdentityFrom(request);
  ProjectionApplyDiscardResultDTO result{
      request.apply_id,
      request.apply_generation,
      request.projection_stage_generation,
      request.candidate_projection_revision,
      request.expected_predecessor_projection_revision,
      ProjectionApplyDiscardResultState::kFailedProjectionUncertain,
      ZeroNativeTransitionEvidence(
          identity, ProjectionTransitionEvidenceState::kUncertain, error),
      error,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent failed named Apply discard");
  }
  return result;
}

ProjectionApplyDiscardResultDTO
FailedDiscardResult(const NamedApplyStageState &stage,
                    ProjectionTransitionEvidenceDTO evidence, std::string code,
                    std::string message) {
  ErrorDTO error{std::move(code), std::move(message), {}};
  ProjectionApplyDiscardResultDTO result{
      stage.apply_id,
      stage.apply_generation,
      stage.stage_generation,
      stage.candidate,
      stage.predecessor,
      ProjectionApplyDiscardResultState::kFailedProjectionUncertain,
      std::move(evidence),
      std::move(error),
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent failed native named Apply discard");
  }
  return result;
}

ProjectionApplyFinalizeResultDTO
FailedNamedFinalizeResult(const NamedApplyStageState &stage,
                          const ProjectionApplyFinalizeDTO &request,
                          const ProjectionApplyFinalizeResultState state,
                          std::string code, std::string message) {
  if (state == ProjectionApplyFinalizeResultState::kFinalized) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "failed Finalize helper received success state");
  }
  const ErrorDTO error{std::move(code), std::move(message), {}};
  const auto evidence_state =
      state == ProjectionApplyFinalizeResultState::kStale
          ? ProjectionTransitionEvidenceState::kActivatedAndRetired
          : ProjectionTransitionEvidenceState::kUncertain;
  ProjectionApplyFinalizeResultDTO result{
      request.apply_id,
      request.apply_generation,
      request.projection_stage_generation,
      request.candidate_projection_revision,
      request.publication_receipt,
      state,
      std::nullopt,
      ZeroNativeTransitionEvidence(
          stage, evidence_state,
          evidence_state == ProjectionTransitionEvidenceState::kUncertain
              ? std::optional<ErrorDTO>(error)
              : std::nullopt),
      ZeroNativeNamedApplyHealth(
          stage, ProjectionHealthState::kUnavailable,
          ErrorDTO{
              "candidate_projection_health_unavailable",
              "candidate health is not authoritative before Finalize",
              {},
          }),
      error,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent failed named Apply Finalize");
  }
  return result;
}

ProjectionApplyFinalizeResultDTO
FailedNamedFinalizeResult(const NamedApplyStageState &stage,
                          const ProjectionApplyFinalizeDTO &request,
                          const ProjectionApplyFinalizeResultState state,
                          ProjectionTransitionEvidenceDTO evidence,
                          ProjectionAggregateHealthDTO candidate_health,
                          std::string code, std::string message) {
  if (state == ProjectionApplyFinalizeResultState::kFinalized ||
      request.apply_id != stage.apply_id ||
      request.apply_generation != stage.apply_generation ||
      request.projection_stage_generation != stage.stage_generation ||
      request.candidate_projection_revision != stage.candidate ||
      !evidence.matches_apply(stage.apply_id, stage.apply_generation,
                              stage.stage_generation, stage.predecessor,
                              stage.candidate) ||
      !candidate_health.matches_named_apply(
          stage.apply_id, stage.apply_generation, stage.stage_generation,
          stage.candidate)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "failed native Finalize helper received contradictory identity or "
        "state");
  }
  ErrorDTO error{std::move(code), std::move(message), {}};
  ProjectionApplyFinalizeResultDTO result{
      request.apply_id,
      request.apply_generation,
      request.projection_stage_generation,
      request.candidate_projection_revision,
      request.publication_receipt,
      state,
      std::nullopt,
      std::move(evidence),
      std::move(candidate_health),
      std::move(error),
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent failed native named Apply Finalize");
  }
  return result;
}

ProjectionApplyStageResultDTO
RejectedStageResult(const ProjectionApplyStageDTO &request, std::string code,
                    std::string message) {
  ProjectionApplyStageResultDTO result{
      request.apply_id,
      request.apply_generation,
      request.expected_active_projection_revision,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ProjectionApplyStageResultState::kRejected,
      ErrorDTO{std::move(code), std::move(message), {}},
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent rejected named Apply stage result");
  }
  return result;
}

void InsertNamedApplyStage(sqlite3 *database,
                           const NamedApplyStageState &stage) {
  Statement insert(
      database, "INSERT INTO logical_scene_named_apply_stages("
                "lineage_generation,apply_id,apply_generation,"
                "projection_stage_generation,"
                "predecessor_retained_bundle_id,predecessor_graph_revision,"
                "predecessor_scene_id,predecessor_scene_revision_id,"
                "predecessor_scene_content_hash,candidate_retained_bundle_id,"
                "candidate_graph_revision,candidate_scene_id,"
                "candidate_scene_revision_id,candidate_scene_content_hash,"
                "transition_row_count,transition_evidence_state) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,'staged')");
  insert.Bind(1, CheckedSafeCounter(stage.lineage_generation,
                                    "scene lineage generation"));
  insert.Bind(2, stage.apply_id.value());
  insert.Bind(3, CheckedSafeCounter(stage.apply_generation.value(),
                                    "named Apply generation"));
  insert.Bind(4, CheckedSafeCounter(stage.stage_generation.value(),
                                    "projection stage generation"));
  insert.Bind(5, stage.predecessor.retained_bundle_id.value());
  insert.Bind(6, stage.predecessor.graph_revision.value());
  insert.Bind(7, stage.predecessor.scene_revision.scene_id.value());
  insert.Bind(8,
              CheckedSafeCounter(stage.predecessor.scene_revision.revision_id,
                                 "predecessor scene revision"));
  insert.Bind(9, stage.predecessor.scene_revision.content_hash.value());
  insert.Bind(10, stage.candidate.retained_bundle_id.value());
  insert.Bind(11, stage.candidate.graph_revision.value());
  insert.Bind(12, stage.candidate.scene_revision.scene_id.value());
  insert.Bind(13, CheckedSafeCounter(stage.candidate.scene_revision.revision_id,
                                     "candidate scene revision"));
  insert.Bind(14, stage.candidate.scene_revision.content_hash.value());
  if (insert.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply stage insert unexpectedly returned a row");
  }
}

void InsertNativeApplyIntent(
    sqlite3 *database, const NamedApplyStageState &stage,
    const LogicalSceneSnapshotDTO &frozen_snapshot,
    const std::vector<core::ProjectionTransitionRowDTO> &transition_rows) {
  if (!stage.predecessor.valid() || !stage.candidate.valid() ||
      stage.candidate.scene_revision != stage.predecessor.scene_revision ||
      !frozen_snapshot.has_exact_identity(
          {stage.candidate.scene_revision, frozen_snapshot.cell_id}) ||
      transition_rows.size() > kMaximumNativeProjectionTransitionRows ||
      !core::CanonicalProjectionTransitionRows(transition_rows) ||
      std::any_of(transition_rows.begin(), transition_rows.end(),
                  [&](const auto &row) {
                    return !row.matches_predecessor(stage.predecessor);
                  })) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot persist an invalid native Apply intent");
  }
  const auto snapshot_json =
      logical_scene_codec_internal::EncodeSnapshot(frozen_snapshot);
  if (snapshot_json.empty() ||
      snapshot_json.size() > kMaximumLogicalSceneTransactionBytes) {
    throw CanonicalEmptySceneOwnerLimitError(
        "native Apply frozen snapshot exceeds its fixed byte bound");
  }

  Statement intent(
      database,
      "INSERT INTO logical_scene_named_apply_native_intents("
      "lineage_generation,apply_id,apply_generation,"
      "projection_stage_generation,predecessor_retained_bundle_id,"
      "predecessor_graph_revision,predecessor_scene_id,"
      "predecessor_scene_revision_id,predecessor_scene_content_hash,"
      "candidate_retained_bundle_id,candidate_graph_revision,"
      "candidate_scene_id,candidate_scene_revision_id,"
      "candidate_scene_content_hash,frozen_snapshot_json,"
      "native_transition_row_count) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  intent.Bind(1, CheckedSafeCounter(stage.lineage_generation,
                                    "scene lineage generation"));
  intent.Bind(2, stage.apply_id.value());
  intent.Bind(3, CheckedSafeCounter(stage.apply_generation.value(),
                                    "native Apply generation"));
  intent.Bind(4, CheckedSafeCounter(stage.stage_generation.value(),
                                    "native projection stage generation"));
  intent.Bind(5, stage.predecessor.retained_bundle_id.value());
  intent.Bind(6, stage.predecessor.graph_revision.value());
  intent.Bind(7, stage.predecessor.scene_revision.scene_id.value());
  intent.Bind(8,
              CheckedSafeCounter(stage.predecessor.scene_revision.revision_id,
                                 "native predecessor scene revision"));
  intent.Bind(9, stage.predecessor.scene_revision.content_hash.value());
  intent.Bind(10, stage.candidate.retained_bundle_id.value());
  intent.Bind(11, stage.candidate.graph_revision.value());
  intent.Bind(12, stage.candidate.scene_revision.scene_id.value());
  intent.Bind(13, CheckedSafeCounter(stage.candidate.scene_revision.revision_id,
                                     "native candidate scene revision"));
  intent.Bind(14, stage.candidate.scene_revision.content_hash.value());
  intent.Bind(15, snapshot_json);
  intent.Bind(16, static_cast<std::int64_t>(transition_rows.size()));
  if (intent.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native Apply intent insert unexpectedly returned a row");
  }

  for (std::size_t ordinal = 0U; ordinal < transition_rows.size(); ++ordinal) {
    const auto &row = transition_rows[ordinal];
    const auto &binding = row.native_runtime_binding;
    Statement insert(
        database,
        "INSERT INTO logical_scene_named_apply_native_transition_rows("
        "apply_id,apply_generation,projection_stage_generation,"
        "transition_ordinal,provider_binding_id,provider_binding_revision,"
        "runtime_unit_id,runtime_epoch,disposition,"
        "expected_local_retained_bundle_id,expected_local_graph_revision,"
        "expected_local_scene_id,expected_local_scene_revision_id,"
        "expected_local_scene_content_hash) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    insert.Bind(1, stage.apply_id.value());
    insert.Bind(2, CheckedSafeCounter(stage.apply_generation.value(),
                                      "native Apply generation"));
    insert.Bind(3, CheckedSafeCounter(stage.stage_generation.value(),
                                      "native projection stage generation"));
    insert.Bind(4, static_cast<std::int64_t>(ordinal));
    insert.Bind(5, binding.provider_binding_id.value());
    insert.Bind(6, binding.provider_binding_revision.value());
    insert.Bind(7, binding.runtime_unit_id.value());
    insert.Bind(8, binding.runtime_epoch.value());
    insert.Bind(9, core::to_string(row.disposition));
    if (row.expected_local_active.has_value()) {
      const auto &expected = *row.expected_local_active;
      insert.Bind(10, expected.retained_bundle_id.value());
      insert.Bind(11, expected.graph_revision.value());
      insert.Bind(12, expected.scene_revision.scene_id.value());
      insert.Bind(13, CheckedSafeCounter(expected.scene_revision.revision_id,
                                         "native expected scene revision"));
      insert.Bind(14, expected.scene_revision.content_hash.value());
    } else {
      for (int index = 10; index <= 14; ++index) {
        insert.BindNull(index);
      }
    }
    if (insert.Step()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native Apply transition-row insert unexpectedly returned a row");
    }
  }
}

void InsertNativeActivationSelection(sqlite3 *database,
                                     const NamedApplyStageState &stage) {
  Statement insert(
      database, "INSERT INTO logical_scene_named_apply_activation_selections("
                "apply_id,apply_generation,projection_stage_generation,"
                "candidate_retained_bundle_id,candidate_graph_revision,"
                "candidate_scene_id,candidate_scene_revision_id,"
                "candidate_scene_content_hash) VALUES(?,?,?,?,?,?,?,?)");
  insert.Bind(1, stage.apply_id.value());
  insert.Bind(2, CheckedSafeCounter(stage.apply_generation.value(),
                                    "native Apply generation"));
  insert.Bind(3, CheckedSafeCounter(stage.stage_generation.value(),
                                    "native projection stage generation"));
  insert.Bind(4, stage.candidate.retained_bundle_id.value());
  insert.Bind(5, stage.candidate.graph_revision.value());
  insert.Bind(6, stage.candidate.scene_revision.scene_id.value());
  insert.Bind(7, CheckedSafeCounter(stage.candidate.scene_revision.revision_id,
                                    "native candidate scene revision"));
  insert.Bind(8, stage.candidate.scene_revision.content_hash.value());
  if (insert.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native activation-selection insert unexpectedly returned a row");
  }
}

void CompactNativeRuntimeRecoveryHistory(
    sqlite3 *database, const DurableState &state,
    const NativeProjectionRecoveryGeneration next_generation) {
  if (!next_generation.nonzero()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native recovery compaction received a zero generation");
  }
  std::optional<std::uint64_t> retained_completed_generation;
  const auto *active_stage = state.ActiveStructuralStage();
  if (active_stage != nullptr) {
    for (const auto &recovery : state.native_runtime_recoveries) {
      if (recovery.structural_lineage_generation ==
              active_stage->lineage_generation &&
          recovery.completion_health.has_value()) {
        retained_completed_generation = recovery.recovery_generation.value();
      }
    }
  }

  Statement open(
      database,
      "UPDATE logical_scene_history_state SET compaction_in_progress=1,"
      "native_recovery_generation_watermark=? WHERE singleton=1 AND "
      "compaction_in_progress=0 AND native_recovery_generation_watermark<?");
  open.Bind(1, CheckedSafeCounter(next_generation.value(),
                                  "native recovery generation"));
  open.Bind(2, CheckedSafeCounter(next_generation.value(),
                                  "native recovery generation"));
  if (open.Step() || sqlite3_changes(database) != 1) {
    throw CanonicalEmptySceneOwnerConflictError(
        "native recovery generation changed before bounded compaction");
  }

  const auto delete_except = [&](const std::string_view table) {
    const auto sql = retained_completed_generation.has_value()
                         ? "DELETE FROM " + std::string(table) +
                               " WHERE recovery_generation<>?"
                         : "DELETE FROM " + std::string(table);
    Statement statement(database, sql);
    if (retained_completed_generation.has_value()) {
      statement.Bind(1, CheckedSafeCounter(*retained_completed_generation,
                                           "retained recovery generation"));
    }
    if (statement.Step()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native recovery compaction delete returned an unexpected row");
    }
  };
  delete_except("logical_scene_native_runtime_recovery_results");
  delete_except("logical_scene_native_runtime_recovery_completions");
  delete_except("logical_scene_native_runtime_recovery_bindings");
  delete_except("logical_scene_native_runtime_recovery_intents");
}

void CloseNativeRuntimeRecoveryCompaction(sqlite3 *database) {
  Exec(database, "UPDATE logical_scene_history_state SET "
                 "compaction_in_progress=0 WHERE singleton=1");
}

void InsertNativeRuntimeRecoveryIntent(
    sqlite3 *database,
    const NativeProjectionRecoveryGeneration recovery_generation,
    const std::uint64_t structural_lineage_generation,
    const ActiveProjectionRuntimeSetDTO &runtime_set,
    const std::optional<std::uint64_t> recovered_transaction_sequence) {
  if (!recovery_generation.nonzero() || !runtime_set.valid() ||
      runtime_set.native_runtime_bindings.empty() ||
      runtime_set.native_runtime_bindings.size() >
          kMaximumNativeProjectionTransitionRows) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot persist an invalid native runtime recovery intent");
  }
  Statement intent(
      database,
      "INSERT INTO logical_scene_native_runtime_recovery_intents("
      "recovery_generation,structural_lineage_generation,"
      "target_retained_bundle_id,target_graph_revision,target_scene_id,"
      "target_scene_revision_id,target_scene_content_hash,"
      "native_binding_count,recovered_transaction_sequence) "
      "VALUES(?,?,?,?,?,?,?,?,?)");
  intent.Bind(1, CheckedSafeCounter(recovery_generation.value(),
                                    "native recovery generation"));
  intent.Bind(2, CheckedSafeCounter(structural_lineage_generation,
                                    "native recovery structural lineage"));
  intent.Bind(
      3, runtime_set.active_projection_revision.retained_bundle_id.value());
  intent.Bind(4, runtime_set.active_projection_revision.graph_revision.value());
  intent.Bind(
      5,
      runtime_set.active_projection_revision.scene_revision.scene_id.value());
  intent.Bind(
      6, CheckedSafeCounter(
             runtime_set.active_projection_revision.scene_revision.revision_id,
             "native recovery target scene revision"));
  intent.Bind(7, runtime_set.active_projection_revision.scene_revision
                     .content_hash.value());
  intent.Bind(
      8, static_cast<std::int64_t>(runtime_set.native_runtime_bindings.size()));
  if (recovered_transaction_sequence.has_value()) {
    intent.Bind(9, CheckedSafeCounter(*recovered_transaction_sequence,
                                      "recovered transaction sequence"));
  } else {
    intent.BindNull(9);
  }
  if (intent.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native runtime recovery intent insert returned a row");
  }

  for (std::size_t ordinal = 0U;
       ordinal < runtime_set.native_runtime_bindings.size(); ++ordinal) {
    const auto &binding = runtime_set.native_runtime_bindings[ordinal];
    Statement insert(
        database, "INSERT INTO logical_scene_native_runtime_recovery_bindings("
                  "recovery_generation,binding_ordinal,provider_binding_id,"
                  "provider_binding_revision,runtime_unit_id,runtime_epoch) "
                  "VALUES(?,?,?,?,?,?)");
    insert.Bind(1, CheckedSafeCounter(recovery_generation.value(),
                                      "native recovery generation"));
    insert.Bind(2, static_cast<std::int64_t>(ordinal));
    insert.Bind(3, binding.provider_binding_id.value());
    insert.Bind(4, binding.provider_binding_revision.value());
    insert.Bind(5, binding.runtime_unit_id.value());
    insert.Bind(6, binding.runtime_epoch.value());
    if (insert.Step()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native runtime recovery binding insert returned a row");
    }
  }
}

void InsertNativeRuntimeRecoveryCompletion(
    sqlite3 *database,
    const NativeProjectionRecoveryGeneration recovery_generation,
    const ProjectionRevisionDTO &target,
    const std::vector<core::ProjectionHealthDTO> &health) {
  if (!recovery_generation.nonzero() || !target.valid() || health.empty() ||
      health.size() > kMaximumNativeProjectionTransitionRows) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot persist an invalid native runtime recovery completion");
  }
  for (std::size_t ordinal = 0U; ordinal < health.size(); ++ordinal) {
    const auto &row = health[ordinal];
    if (!row.healthy_for_target(target) || row.source_sequence == 0U ||
        !row.native_projection_generation.has_value() ||
        !row.projection_content_digest.has_value() ||
        (ordinal != 0U && !core::CanonicalProjectionNativeBindingLess(
                              health[ordinal - 1U].native_runtime_binding,
                              row.native_runtime_binding))) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native runtime recovery completion lacks exact healthy evidence");
    }
    Statement insert(
        database, "INSERT INTO logical_scene_native_runtime_recovery_results("
                  "recovery_generation,binding_ordinal,source_sequence,"
                  "native_projection_generation,projection_content_digest) "
                  "VALUES(?,?,?,?,?)");
    insert.Bind(1, CheckedSafeCounter(recovery_generation.value(),
                                      "native recovery generation"));
    insert.Bind(2, static_cast<std::int64_t>(ordinal));
    insert.Bind(3, CheckedSafeCounter(row.source_sequence,
                                      "native recovery source sequence"));
    insert.Bind(4, CheckedSafeCounter(*row.native_projection_generation,
                                      "native projection generation"));
    insert.Bind(5, row.projection_content_digest->value());
    if (insert.Step()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native runtime recovery result insert returned a row");
    }
  }
  Statement completion(
      database, "INSERT INTO logical_scene_native_runtime_recovery_completions("
                "recovery_generation,result_count) VALUES(?,?)");
  completion.Bind(1, CheckedSafeCounter(recovery_generation.value(),
                                        "native recovery generation"));
  completion.Bind(2, static_cast<std::int64_t>(health.size()));
  if (completion.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native runtime recovery completion insert returned a row");
  }
}

void InsertNamedApplyActivation(sqlite3 *database,
                                const NamedApplyStageState &stage) {
  Statement insert(database,
                   "INSERT INTO logical_scene_named_apply_activations("
                   "apply_id,apply_generation,projection_stage_generation,"
                   "transition_row_count,transition_evidence_state) "
                   "VALUES(?,?,?,0,'activated_and_retired')");
  insert.Bind(1, stage.apply_id.value());
  insert.Bind(2, CheckedSafeCounter(stage.apply_generation.value(),
                                    "named Apply generation"));
  insert.Bind(3, CheckedSafeCounter(stage.stage_generation.value(),
                                    "projection stage generation"));
  if (insert.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply activation insert unexpectedly returned a row");
  }
}

void InsertNamedApplyDiscard(sqlite3 *database,
                             const NamedApplyStageState &stage) {
  Statement insert(database,
                   "INSERT INTO logical_scene_named_apply_discards("
                   "apply_id,apply_generation,projection_stage_generation,"
                   "transition_row_count,transition_evidence_state) "
                   "VALUES(?,?,?,0,'aborted')");
  insert.Bind(1, stage.apply_id.value());
  insert.Bind(2, CheckedSafeCounter(stage.apply_generation.value(),
                                    "named Apply generation"));
  insert.Bind(3, CheckedSafeCounter(stage.stage_generation.value(),
                                    "projection stage generation"));
  if (insert.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply discard insert unexpectedly returned a row");
  }
}

void InsertNamedApplyFinalization(sqlite3 *database,
                                  const ProjectionApplyFinalizeDTO &request) {
  const auto &publication_request = request.publication_receipt.request;
  if (!publication_request.expected_previous_publication_generation
           .has_value()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply receipt has no predecessor publication generation");
  }
  Statement insert(
      database,
      "INSERT INTO logical_scene_named_apply_finalizations("
      "apply_id,apply_generation,projection_stage_generation,"
      "expected_previous_publication_generation,publication_generation,"
      "private_materialization_content_digest,"
      "obsolete_command_surfaces_retired_evidence_digest,"
      "durable_public_pointer_content_digest,transition_row_count,"
      "transition_evidence_state,candidate_health_row_count,"
      "candidate_health_state) "
      "VALUES(?,?,?,?,?,?,?,?,0,'finalized',0,'healthy')");
  insert.Bind(1, request.apply_id.value());
  insert.Bind(2, CheckedSafeCounter(request.apply_generation.value(),
                                    "named Apply generation"));
  insert.Bind(3, CheckedSafeCounter(request.projection_stage_generation.value(),
                                    "projection stage generation"));
  insert.Bind(
      4,
      CheckedSafeCounter(
          publication_request.expected_previous_publication_generation->value(),
          "previous publication generation"));
  insert.Bind(5, CheckedSafeCounter(
                     request.publication_receipt.publication_generation.value(),
                     "publication generation"));
  insert.Bind(
      6, publication_request.private_materialization_content_digest.value());
  insert.Bind(
      7,
      publication_request
          .obsolete_managed_command_surfaces_retired_evidence_digest.value());
  insert.Bind(8, request.publication_receipt
                     .durable_public_pointer_content_digest.value());
  if (insert.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply Finalize insert unexpectedly returned a row");
  }
}

ProjectionBootstrapFinalizeResultDTO
FailedFinalizeResult(const ProjectionBootstrapFinalizeDTO &request,
                     const AcceptedState &accepted,
                     const ProjectionBootstrapFinalizeResultState state,
                     std::string code, std::string message) {
  ProjectionBootstrapFinalizeResultDTO result{
      request.bootstrap_id,
      request.bootstrap_generation,
      request.canonical_empty_target,
      request.publication_receipt,
      state,
      ZeroNativeEvidence(accepted),
      std::nullopt,
      ErrorDTO{std::move(code), std::move(message), {}},
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent failed canonical-empty Finalize result");
  }
  return result;
}

} // namespace

std::string_view
ToString(const CanonicalEmptySceneBootstrapPhase phase) noexcept {
  switch (phase) {
  case CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced:
    return "initial_command_fenced";
  case CanonicalEmptySceneBootstrapPhase::kAccepted:
    return "accepted";
  case CanonicalEmptySceneBootstrapPhase::kReadyToPublish:
    return "ready_to_publish";
  case CanonicalEmptySceneBootstrapPhase::kFinalized:
    return "finalized";
  case CanonicalEmptySceneBootstrapPhase::kNamedApplyStaged:
    return "named_apply_staged";
  case CanonicalEmptySceneBootstrapPhase::kNamedApplyActivated:
    return "named_apply_activated";
  case CanonicalEmptySceneBootstrapPhase::kNamedApplyDiscarded:
    return "named_apply_discarded";
  case CanonicalEmptySceneBootstrapPhase::kNamedApplyFinalized:
    return "named_apply_finalized";
  }
  return {};
}

bool CanonicalEmptySceneAccessStatus::valid() const noexcept {
  switch (phase) {
  case CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced:
  case CanonicalEmptySceneBootstrapPhase::kAccepted:
  case CanonicalEmptySceneBootstrapPhase::kReadyToPublish:
    return !scene_writes_open && !projection_leases_open &&
           !active_projection.has_value();
  case CanonicalEmptySceneBootstrapPhase::kFinalized:
    return scene_writes_open == projection_leases_open &&
           active_projection.has_value() && active_projection->valid();
  case CanonicalEmptySceneBootstrapPhase::kNamedApplyStaged:
  case CanonicalEmptySceneBootstrapPhase::kNamedApplyActivated:
    return !scene_writes_open && !projection_leases_open &&
           active_projection.has_value() && active_projection->valid();
  case CanonicalEmptySceneBootstrapPhase::kNamedApplyDiscarded:
  case CanonicalEmptySceneBootstrapPhase::kNamedApplyFinalized:
    return scene_writes_open == projection_leases_open &&
           active_projection.has_value() && active_projection->valid();
  }
  return false;
}

class CanonicalEmptySceneOwner::Impl final {
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
      throw CanonicalEmptySceneOwnerBusyError(
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
              } else {
                ValidateDatabase(preflight, fixed_query_, admission_digest_);
              }
            }
          }));
    } catch (const CanonicalEmptySceneOwnerError &) {
      throw;
    } catch (const std::exception &error) {
      throw CanonicalEmptySceneOwnerIntegrityError(
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
      throw CanonicalEmptySceneOwnerError(
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
          throw CanonicalEmptySceneOwnerIntegrityError(
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
          transaction.Commit();
          changed_schema = true;
        } else if (version == kNativeProjectionHandoffStoreSchemaVersion) {
          ValidateNativeHandoffDatabase(database_, fixed_query_,
                                        admission_digest_);
          ConfigureDurability(database_);
          Transaction transaction(database_, "BEGIN IMMEDIATE");
          Exec(database_, detail::kEmbeddedBoundedHistoryMigration);
          InsertBoundedHistoryMigrationMetadata(database_);
          transaction.Commit();
          changed_schema = true;
        } else {
          ValidateDatabase(database_, fixed_query_, admission_digest_);
          ConfigureDurability(database_);
        }
      }
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
      if (changed_schema || recovered_scene_transaction) {
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

  [[nodiscard]] sqlite3 *database() const {
    if (database_ == nullptr) {
      throw CanonicalEmptySceneOwnerClosedError(
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
      throw CanonicalEmptySceneOwnerConflictError(
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
  mutable std::mutex mutex_;
  sqlite3 *database_{};
  mutable std::optional<SceneCommitStateCache> scene_commit_state_cache_;
};

CanonicalEmptySceneOwner::CanonicalEmptySceneOwner(
    std::filesystem::path database_path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader)
    : impl_(std::make_unique<Impl>(std::move(database_path), admission,
                                   publication_reader)) {}

CanonicalEmptySceneOwner::CanonicalEmptySceneOwner(
    std::filesystem::path database_path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader,
    core::IKernelCellAdmissionPort &cell_admission)
    : impl_(std::make_unique<Impl>(std::move(database_path), admission,
                                   publication_reader, cell_admission)) {}

CanonicalEmptySceneOwner::CanonicalEmptySceneOwner(
    std::filesystem::path database_path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source)
    : impl_(std::make_unique<Impl>(std::move(database_path), admission,
                                   publication_reader,
                                   retained_bundle_source)) {}

CanonicalEmptySceneOwner::CanonicalEmptySceneOwner(
    std::filesystem::path database_path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
    const core::IPlannerSceneProjectionResolver &projection_resolver)
    : impl_(std::make_unique<Impl>(std::move(database_path), admission,
                                   publication_reader, retained_bundle_source,
                                   projection_resolver)) {}

CanonicalEmptySceneOwner::CanonicalEmptySceneOwner(
    std::filesystem::path database_path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
    const core::IPlannerSceneProjectionResolver &projection_resolver,
    core::IKernelCellAdmissionPort &cell_admission)
    : impl_(std::make_unique<Impl>(
          std::move(database_path), admission, publication_reader,
          retained_bundle_source, projection_resolver, cell_admission)) {}

CanonicalEmptySceneOwner::~CanonicalEmptySceneOwner() = default;
CanonicalEmptySceneOwner::CanonicalEmptySceneOwner(
    CanonicalEmptySceneOwner &&) noexcept = default;
CanonicalEmptySceneOwner &CanonicalEmptySceneOwner::operator=(
    CanonicalEmptySceneOwner &&) noexcept = default;

const std::filesystem::path &
CanonicalEmptySceneOwner::database_path() const noexcept {
  return impl_->database_path_;
}

const ProjectionBootstrapPrerequisiteQueryDTO &
CanonicalEmptySceneOwner::fixed_query() const noexcept {
  return impl_->fixed_query_;
}

ProjectionBootstrapPrerequisiteStatusDTO
CanonicalEmptySceneOwner::ObserveProjectionBootstrapPrerequisites(
    const ProjectionBootstrapPrerequisiteQueryDTO &query) const {
  if (!query.valid()) {
    throw std::invalid_argument(
        "bootstrap prerequisite observation query is invalid");
  }
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadStateTransaction();
  const auto access = AccessStatus(state);
  if (query != impl_->fixed_query_) {
    return {
        query,
        ProjectionBootstrapPrerequisiteState::kUnavailable,
        !access.scene_writes_open,
        !access.projection_leases_open,
        std::nullopt,
        ErrorDTO{
            "projection_bootstrap_prerequisite_mismatch",
            "query does not match this scene owner's fixed admitted cell",
            {},
        },
    };
  }
  if (access.active_projection.has_value()) {
    return {
        query,
        ProjectionBootstrapPrerequisiteState::kConflictingActiveProjection,
        !access.scene_writes_open,
        !access.projection_leases_open,
        access.active_projection,
        ErrorDTO{
            "conflicting_active_projection",
            "the canonical-empty scene has already been finalized",
            {},
        },
    };
  }
  ProjectionBootstrapPrerequisiteStatusDTO ready{
      query,        ProjectionBootstrapPrerequisiteState::kReadyCommandFenced,
      true,         true,
      std::nullopt, std::nullopt,
  };
  if (!ready.ready_for(query)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent command-fenced bootstrap prerequisites");
  }
  return ready;
}

ProjectionBootstrapAcceptedDTO
CanonicalEmptySceneOwner::BeginProjectionBootstrap(
    const ProjectionBootstrapIntentDTO &intent) {
  ValidateCanonicalEmptyIntent(intent, impl_->fixed_query_,
                               impl_->admitted_bundle_);
  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto *database = impl_->database();
  Transaction transaction(database, "BEGIN IMMEDIATE");
  auto state = ReadCompleteDurableState(database, impl_->fixed_query_);
  if (state.accepted.has_value()) {
    if (state.accepted->bootstrap_id != intent.bootstrap_id ||
        state.accepted->bootstrap_generation != intent.bootstrap_generation) {
      throw CanonicalEmptySceneOwnerConflictError(
          "a different canonical-empty bootstrap already owns this scene "
          "lineage");
    }
    transaction.Commit();
    return AcceptedResult(*state.accepted);
  }
  if (state.phase() !=
          CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced ||
      AccessStatus(state).scene_writes_open ||
      AccessStatus(state).projection_leases_open ||
      AccessStatus(state).active_projection.has_value()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "Begin atomic prerequisite recheck is not command-fenced empty");
  }
  const auto accepted = MintAcceptedState(intent, impl_->fixed_query_);
  InsertAcceptance(database, accepted);
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  if (!state.accepted.has_value() || state.rebuilt ||
      state.finalization_receipt.has_value() ||
      state.accepted->target != accepted.target) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "durable Begin acceptance did not replay exactly");
  }
  transaction.Commit();
  return AcceptedResult(*state.accepted);
}

ProjectionBootstrapRebuildResultDTO
CanonicalEmptySceneOwner::RebuildProjectionBootstrap(
    const ProjectionBootstrapRebuildRequestDTO &request) {
  if (!request.valid()) {
    throw std::invalid_argument("bootstrap Rebuild request is invalid");
  }
  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto *database = impl_->database();
  Transaction transaction(database, "BEGIN IMMEDIATE");
  auto state = ReadCompleteDurableState(database, impl_->fixed_query_);
  if (!state.accepted.has_value() ||
      request.bootstrap_id != state.accepted->bootstrap_id ||
      request.bootstrap_generation != state.accepted->bootstrap_generation ||
      request.canonical_empty_target != state.accepted->target) {
    throw CanonicalEmptySceneOwnerConflictError(
        "Rebuild does not match the exact durable Begin acceptance");
  }
  if (!state.rebuilt) {
    InsertRebuild(database, *state.accepted);
    state = ReadCompleteDurableState(database, impl_->fixed_query_);
  }
  if (!state.rebuilt) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "zero-native Rebuild was not durably recorded");
  }
  const auto evidence = ZeroNativeEvidence(*state.accepted);
  ProjectionBootstrapRebuildResultDTO result{
      state.accepted->bootstrap_id,
      state.accepted->bootstrap_generation,
      state.accepted->target,
      ProjectionBootstrapRebuildResultState::kReadyToPublish,
      evidence,
      std::nullopt,
  };
  if (!result.presence_valid() || !evidence.rebuild_results.empty() ||
      !evidence.candidate_aggregate_health.native_projection_health.empty()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent exact healthy zero-native Rebuild evidence");
  }
  transaction.Commit();
  return result;
}

ProjectionBootstrapStatusDTO
CanonicalEmptySceneOwner::ProjectionBootstrapStatus(
    const ProjectionBootstrapStatusQueryDTO &query) const {
  if (!query.valid()) {
    throw std::invalid_argument("bootstrap status query is invalid");
  }
  ValidateBoundedIdentity(query.bootstrap_id.value(), "bootstrap_id");
  static_cast<void>(CheckedSafeCounter(query.bootstrap_generation.value(),
                                       "bootstrap_generation"));
  std::lock_guard lock(impl_->mutex_);
  auto result =
      StatusResult(impl_->ReadSceneCommitState().durable_state, query);
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "durable state cannot form a valid bootstrap status");
  }
  return result;
}

ProjectionBootstrapFinalizeResultDTO
CanonicalEmptySceneOwner::FinalizeProjectionBootstrap(
    const ProjectionBootstrapFinalizeDTO &request) {
  if (!request.valid()) {
    throw std::invalid_argument("bootstrap Finalize request is invalid");
  }
  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto state = impl_->ReadStateTransaction();
  if (!state.accepted.has_value() || !state.rebuilt) {
    throw CanonicalEmptySceneOwnerConflictError(
        "Finalize requires the exact durable zero-native Rebuild evidence");
  }
  ValidateFinalizeRequest(request, *state.accepted);
  if (state.finalization_receipt.has_value()) {
    if (*state.finalization_receipt != request.publication_receipt) {
      throw CanonicalEmptySceneOwnerConflictError(
          "Finalize replay carries a different publication receipt");
    }
    ProjectionBootstrapFinalizeResultDTO replay{
        state.accepted->bootstrap_id,
        state.accepted->bootstrap_generation,
        state.accepted->target,
        *state.finalization_receipt,
        ProjectionBootstrapFinalizeResultState::kFinalized,
        ZeroNativeEvidence(*state.accepted),
        ZeroNativeHealth(*state.accepted),
        std::nullopt,
    };
    if (!replay.presence_valid()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "durable Finalize replay is invalid");
    }
    return replay;
  }

  core::Result<ActiveBundlePublicationReceiptDTO> durable = [&] {
    try {
      return impl_->publication_reader_.ReadActiveBundlePublication(
          request.publication_receipt.publication_generation);
    } catch (const std::exception &error) {
      return core::Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "active_bundle_publication_read_uncertain",
          error.what(),
          {},
      });
    } catch (...) {
      return core::Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "active_bundle_publication_read_uncertain",
          "publication reader threw a non-standard exception",
          {},
      });
    }
  }();
  if (!durable.ok()) {
    // Only the publication owner's definitive head answers may prove this
    // receipt stale.  Its production implementation returns integrity,
    // closure, and storage failures as Result errors rather than throwing;
    // treating an unknown/failed read as stale would falsely claim that an
    // authoritative current-head comparison happened.
    const auto &read_error_code = durable.error().code;
    const bool definitively_not_current =
        read_error_code == "missing_reference" ||
        read_error_code == "stale_generation" ||
        read_error_code == "publication_generation_not_current";
    const bool uncertain = !definitively_not_current;
    return FailedFinalizeResult(
        request, *state.accepted,
        uncertain
            ? ProjectionBootstrapFinalizeResultState::kFailedProjectionUncertain
            : ProjectionBootstrapFinalizeResultState::kStale,
        uncertain ? "active_bundle_publication_read_uncertain"
                  : "active_bundle_publication_not_current",
        uncertain ? "the current publication head could not be read "
                    "authoritatively"
                  : "the requested publication generation is not the current "
                    "durable head");
  }
  if (durable.value() != request.publication_receipt) {
    return FailedFinalizeResult(
        request, *state.accepted,
        ProjectionBootstrapFinalizeResultState::kStale,
        "active_bundle_publication_receipt_mismatch",
        "the independently reread current receipt is not byte-equivalent");
  }

  auto *database = impl_->database();
  Transaction transaction(database, "BEGIN IMMEDIATE");
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  if (!state.accepted.has_value() || !state.rebuilt) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "durable bootstrap state changed before Finalize commit");
  }
  ValidateFinalizeRequest(request, *state.accepted);
  if (!state.finalization_receipt.has_value()) {
    InsertFinalization(database, request);
    state = ReadCompleteDurableState(database, impl_->fixed_query_);
  }
  if (!state.finalization_receipt.has_value() ||
      *state.finalization_receipt != request.publication_receipt) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "Finalize receipt was not durably recorded exactly");
  }
  transaction.Commit();

  ProjectionBootstrapFinalizeResultDTO result{
      state.accepted->bootstrap_id,
      state.accepted->bootstrap_generation,
      state.accepted->target,
      *state.finalization_receipt,
      ProjectionBootstrapFinalizeResultState::kFinalized,
      ZeroNativeEvidence(*state.accepted),
      ZeroNativeHealth(*state.accepted),
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "durable Finalize state cannot form a valid result");
  }
  return result;
}

ProjectionApplyStageResultDTO
CanonicalEmptySceneOwner::StageCandidateProjection(
    const ProjectionApplyStageDTO &request) {
  if (!request.valid()) {
    throw std::invalid_argument("named Apply Stage request is invalid");
  }
  ValidateBoundedIdentity(request.apply_id.value(), "Apply ID");
  static_cast<void>(
      CheckedSafeCounter(request.apply_generation.value(), "Apply generation"));
  if (impl_->retained_bundle_source_ == nullptr) {
    return RejectedStageResult(
        request, "named_apply_retained_source_unavailable",
        "this scene owner was not provisioned with the verified retained "
        "bundle source required by named Apply");
  }

  std::shared_ptr<const deployment::RetainedBundle> predecessor;
  std::shared_ptr<const deployment::RetainedBundle> candidate;
  try {
    predecessor = impl_->retained_bundle_source_->get_verified(
        request.predecessor_retained_bundle.retained_bundle_id);
    candidate = impl_->retained_bundle_source_->get_verified(
        request.candidate_retained_bundle.retained_bundle_id);
  } catch (const std::exception &error) {
    return RejectedStageResult(
        request, "named_apply_retained_bundle_unavailable",
        "the scene owner could not independently reload the retained "
        "bundles: " +
            std::string(error.what()));
  } catch (...) {
    return RejectedStageResult(
        request, "named_apply_retained_bundle_unavailable",
        "the scene owner could not independently reload the retained "
        "bundles");
  }
  if (predecessor == nullptr || candidate == nullptr ||
      !deployment::verify_retained_bundle_identity(*predecessor) ||
      !deployment::verify_retained_bundle_identity(*candidate) ||
      predecessor->value() != request.predecessor_retained_bundle ||
      candidate->value() != request.candidate_retained_bundle ||
      predecessor->value().cell_graph.cell_id != impl_->fixed_query_.cell_id ||
      candidate->value().cell_graph.cell_id != impl_->fixed_query_.cell_id ||
      !TransitionRowsMatchVerifiedBundles(request.native_transition_rows,
                                          predecessor->value(),
                                          candidate->value())) {
    return RejectedStageResult(
        request, "named_apply_retained_bundle_mismatch",
        "the complete independently verified retained bundles do not match "
        "the request, native transition set, or this logical-scene lineage");
  }

  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto *database = impl_->database();
  auto state = impl_->ReadStateTransaction();
  std::optional<NamedApplyStageState> stage;
  bool new_stage = false;
  if (const auto *existing =
          state.FindNamedApply(request.apply_id, request.apply_generation);
      existing != nullptr) {
    if (existing->predecessor != request.expected_active_projection_revision ||
        existing->candidate.retained_bundle_id !=
            request.candidate_retained_bundle.retained_bundle_id ||
        existing->candidate.graph_revision !=
            request.candidate_retained_bundle.cell_graph.graph_revision ||
        existing->native_transition_rows != request.native_transition_rows) {
      return RejectedStageResult(
          request, "named_apply_transition_conflict",
          "another exact named Apply transition already owns the scene "
          "quiesce");
    }
    stage = *existing;
  } else {
    if (state.OpenNamedApply() != nullptr) {
      return RejectedStageResult(
          request, "named_apply_transition_conflict",
          "another exact named Apply transition already owns the scene "
          "quiesce");
    }
    const auto access = AccessStatus(state);
    if (!state.accepted.has_value() || !access.scene_writes_open ||
        !access.projection_leases_open ||
        !access.active_projection.has_value() ||
        !state.active_scene_snapshot.has_value() ||
        request.expected_active_projection_revision !=
            *access.active_projection ||
        request.predecessor_retained_bundle.retained_bundle_id !=
            access.active_projection->retained_bundle_id ||
        request.predecessor_retained_bundle.cell_graph.graph_revision !=
            access.active_projection->graph_revision) {
      return RejectedStageResult(
          request, "named_apply_predecessor_mismatch",
          "named Apply expected predecessor is not the exact open active "
          "logical-scene composite");
    }
    if (state.named_apply_stages.size() >= kMaximumNamedApplySceneStages) {
      return RejectedStageResult(
          request, "named_apply_stage_history_exhausted",
          "the logical-scene owner has reached its bounded named Apply "
          "history limit");
    }
    stage = NamedApplyStageState{
        state.named_apply_stages.size() + 1U,
        request.apply_id,
        request.apply_generation,
        ProjectionStageGeneration(1U),
        request.expected_active_projection_revision,
        ProjectionRevisionDTO{
            request.candidate_retained_bundle.retained_bundle_id,
            request.candidate_retained_bundle.cell_graph.graph_revision,
            access.active_projection->scene_revision,
        },
        false,
        false,
        std::nullopt,
        request.native_transition_rows,
        *state.active_scene_snapshot,
        request.native_transition_rows.empty(),
        false,
    };
    new_stage = true;
  }

  if (request.native_transition_rows.empty()) {
    if (stage->discarded) {
      const ErrorDTO error{
          "named_apply_stage_discarded",
          "the exact named Apply stage was already discarded",
          {},
      };
      return CapturedStageFailureResult(
          *stage, ProjectionApplyStageResultState::kRejected,
          ZeroNativeTransitionEvidence(
              *stage, ProjectionTransitionEvidenceState::kAborted),
          error);
    }
    if (stage->activated || stage->activation_selected ||
        stage->finalization_receipt.has_value()) {
      const ErrorDTO error{
          "named_apply_stage_already_selected",
          "the exact named Apply stage has entered forward-only activation",
          {},
      };
      return CapturedStageFailureResult(
          *stage, ProjectionApplyStageResultState::kFailedProjectionUncertain,
          ZeroNativeTransitionEvidence(
              *stage, ProjectionTransitionEvidenceState::kUncertain, error),
          error);
    }
    if (new_stage) {
      Transaction transaction(database, "BEGIN IMMEDIATE");
      const auto current =
          ReadCompleteDurableState(database, impl_->fixed_query_);
      const auto access = AccessStatus(current);
      if (current.OpenNamedApply() != nullptr || !access.scene_writes_open ||
          !access.projection_leases_open ||
          access.active_projection !=
              std::optional<ProjectionRevisionDTO>{stage->predecessor}) {
        return RejectedStageResult(
            request, "named_apply_predecessor_mismatch",
            "logical-scene state changed before durable stage capture");
      }
      InsertNamedApplyStage(database, *stage);
      transaction.Commit();
    }
    return StagedResult(*stage);
  }

  std::vector<NativeTransitionParticipant> participants;
  try {
    participants = ObserveNativeTransitionParticipants(
        request.native_transition_rows, impl_->projection_resolver_);
  } catch (const std::exception &error) {
    if (!new_stage) {
      const ErrorDTO failure{
          "native_projection_status_unavailable", error.what(), {}};
      return CapturedStageFailureResult(
          *stage, ProjectionApplyStageResultState::kFailedProjectionUncertain,
          UnavailableNativeTransitionEvidence(*stage, failure), failure);
    }
    return RejectedStageResult(request, "native_projection_set_unavailable",
                               error.what());
  }

  if (stage->discarded) {
    const ErrorDTO error{
        "named_apply_stage_discarded",
        "the exact named Apply stage was already discarded",
        {},
    };
    try {
      return CapturedStageFailureResult(
          *stage, ProjectionApplyStageResultState::kRejected,
          ObserveNativeTransitionEvidence(
              *stage, participants,
              ProjectionTransitionEvidenceState::kAborted),
          error);
    } catch (const std::exception &status_error) {
      const ErrorDTO failure{
          "native_projection_abort_status_uncertain", status_error.what(), {}};
      return CapturedStageFailureResult(
          *stage, ProjectionApplyStageResultState::kFailedProjectionUncertain,
          UncertainNativeTransitionEvidence(*stage, participants, failure),
          failure);
    }
  }
  if (stage->activated || stage->activation_selected ||
      stage->finalization_receipt.has_value()) {
    const ErrorDTO error{
        "named_apply_stage_already_selected",
        "the exact named Apply stage has entered forward-only activation",
        {},
    };
    try {
      auto observed = ObserveNativeTransitionEvidence(
          *stage, participants,
          stage->finalization_receipt.has_value()
              ? ProjectionTransitionEvidenceState::kFinalized
              : ProjectionTransitionEvidenceState::kActivatedAndRetired);
      return CapturedStageFailureResult(
          *stage, ProjectionApplyStageResultState::kFailedProjectionUncertain,
          NativeTransitionEvidence(
              *stage, ProjectionTransitionEvidenceState::kUncertain,
              std::move(observed.transition_rows), error),
          error);
    } catch (const std::exception &status_error) {
      const ErrorDTO failure{"native_projection_activation_status_uncertain",
                             status_error.what(),
                             {}};
      return CapturedStageFailureResult(
          *stage, ProjectionApplyStageResultState::kFailedProjectionUncertain,
          UncertainNativeTransitionEvidence(*stage, participants, failure),
          failure);
    }
  }

  if (new_stage) {
    for (const auto &participant : participants) {
      const auto &row = participant.transition_row;
      const auto &health = participant.observed_health;
      const bool predecessor_exact =
          row.expected_local_active.has_value()
              ? health.healthy_for_target(stage->predecessor)
              : health.state == ProjectionHealthState::kUninitialized &&
                    !health.active_projection_revision.has_value() &&
                    !health.staged_candidate.has_value() &&
                    !health.error.has_value();
      if (!predecessor_exact) {
        return RejectedStageResult(
            request, "native_projection_predecessor_mismatch",
            "a required native host is not at its exact row predecessor");
      }
    }
    Transaction capture(database, "BEGIN IMMEDIATE");
    const auto current =
        ReadCompleteDurableState(database, impl_->fixed_query_);
    const auto access = AccessStatus(current);
    if (current.OpenNamedApply() != nullptr || !access.scene_writes_open ||
        !access.projection_leases_open ||
        access.active_projection !=
            std::optional<ProjectionRevisionDTO>{stage->predecessor} ||
        !current.active_scene_snapshot.has_value() ||
        *current.active_scene_snapshot != *stage->frozen_snapshot) {
      return RejectedStageResult(
          request, "named_apply_predecessor_mismatch",
          "logical-scene state changed before durable frozen capture");
    }
    InsertNativeApplyIntent(database, *stage, *stage->frozen_snapshot,
                            stage->native_transition_rows);
    capture.Commit();
    state = impl_->ReadStateTransaction();
    const auto *captured =
        state.FindNamedApply(stage->apply_id, stage->apply_generation);
    if (captured == nullptr || captured->stage_completed ||
        captured->native_transition_rows != stage->native_transition_rows ||
        !captured->frozen_snapshot.has_value() ||
        *captured->frozen_snapshot != *stage->frozen_snapshot ||
        AccessStatus(state).scene_writes_open ||
        AccessStatus(state).projection_leases_open) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native Apply capture did not durably close the scene gates");
    }
    stage = *captured;
  }

  const auto uncertain_result =
      [&](std::vector<core::ProjectionTransitionRowEvidenceDTO> rows,
          const std::size_t next_row, ErrorDTO error) {
        rows.reserve(participants.size());
        for (std::size_t index = next_row; index < participants.size();
             ++index) {
          rows.push_back(UncertainTransitionRowEvidence(
              participants[index].transition_row,
              participants[index].observed_health, error));
        }
        return CapturedStageFailureResult(
            *stage, ProjectionApplyStageResultState::kFailedProjectionUncertain,
            NativeTransitionEvidence(
                *stage, ProjectionTransitionEvidenceState::kUncertain,
                std::move(rows), error),
            error);
      };

  std::vector<core::ProjectionTransitionRowEvidenceDTO> staged_rows;
  staged_rows.reserve(participants.size());
  bool clean_stage_rejection = false;
  ErrorDTO stage_error{"native_projection_stage_rejected",
                       "a native projection rejected candidate staging",
                       {}};
  for (std::size_t index = 0U; index < participants.size(); ++index) {
    const auto &participant = participants[index];
    try {
      if (participant.transition_row.disposition ==
          core::ProjectionTransitionDisposition::kRemoved) {
        staged_rows.push_back(PredecessorUnchangedRowEvidence(
            *stage, participant, ReadNativeRetireStatus(*stage, participant)));
        continue;
      }
      auto status = ReadNativeStageStatus(*stage, participant);
      if (status.state == core::NativeProjectionStatusState::kAbsent) {
        const core::NativeProjectionStageDTO native_request{
            stage->apply_id,
            stage->apply_generation,
            stage->stage_generation,
            participant.transition_row,
            stage->predecessor,
            stage->candidate,
            candidate->value().compiled_products.planning_collision_model,
            LogicalSceneSnapshotRoot(
                std::make_shared<const LogicalSceneSnapshotDTO>(
                    *stage->frozen_snapshot)),
        };
        static_cast<void>(
            participant.projection->StageProjection(native_request));
        status = ReadNativeStageStatus(*stage, participant);
      }
      if (status.state == core::NativeProjectionStatusState::kStaged) {
        staged_rows.push_back(StagedRowEvidence(*stage, participant, status));
        continue;
      }
      if (status.state == core::NativeProjectionStatusState::kAbsent ||
          status.state == core::NativeProjectionStatusState::kDiscarded) {
        clean_stage_rejection = true;
        stage_error = {"native_projection_stage_rejected",
                       "a native projection left its candidate stage "
                       "positively absent",
                       {}};
        break;
      }
      throw CanonicalEmptySceneOwnerConflictError(
          "native projection entered a non-staged state before selection");
    } catch (const std::exception &error) {
      const ErrorDTO failure{
          "native_projection_stage_uncertain", error.what(), {}};
      staged_rows.push_back(UncertainTransitionRowEvidence(
          participant.transition_row, participant.observed_health, failure));
      return uncertain_result(std::move(staged_rows), index + 1U, failure);
    } catch (...) {
      const ErrorDTO failure{
          "native_projection_stage_uncertain",
          "native projection stage threw a non-standard exception",
          {}};
      staged_rows.push_back(UncertainTransitionRowEvidence(
          participant.transition_row, participant.observed_health, failure));
      return uncertain_result(std::move(staged_rows), index + 1U, failure);
    }
  }

  if (clean_stage_rejection) {
    std::vector<core::ProjectionTransitionRowEvidenceDTO> aborted_rows;
    aborted_rows.reserve(participants.size());
    for (std::size_t index = 0U; index < participants.size(); ++index) {
      const auto &participant = participants[index];
      try {
        if (participant.transition_row.disposition ==
            core::ProjectionTransitionDisposition::kRemoved) {
          aborted_rows.push_back(PredecessorUnchangedRowEvidence(
              *stage, participant,
              ReadNativeRetireStatus(*stage, participant)));
          continue;
        }
        auto status = ReadNativeStageStatus(*stage, participant);
        if (status.state == core::NativeProjectionStatusState::kStaged) {
          const core::NativeProjectionDiscardDTO discard{
              stage->apply_id,         stage->apply_generation,
              stage->stage_generation, participant.transition_row,
              stage->candidate,
          };
          static_cast<void>(
              participant.projection->DiscardStagedProjection(discard));
          status = ReadNativeStageStatus(*stage, participant);
        }
        aborted_rows.push_back(
            CandidateDiscardedRowEvidence(*stage, participant, status));
      } catch (const std::exception &error) {
        const ErrorDTO failure{
            "native_projection_discard_uncertain", error.what(), {}};
        aborted_rows.push_back(UncertainTransitionRowEvidence(
            participant.transition_row, participant.observed_health, failure));
        return uncertain_result(std::move(aborted_rows), index + 1U, failure);
      } catch (...) {
        const ErrorDTO failure{
            "native_projection_discard_uncertain",
            "native projection discard threw a non-standard exception",
            {}};
        aborted_rows.push_back(UncertainTransitionRowEvidence(
            participant.transition_row, participant.observed_health, failure));
        return uncertain_result(std::move(aborted_rows), index + 1U, failure);
      }
    }
    Transaction completion(database, "BEGIN IMMEDIATE");
    auto current = ReadCompleteDurableState(database, impl_->fixed_query_);
    const auto *persisted =
        current.FindNamedApply(stage->apply_id, stage->apply_generation);
    if (persisted == nullptr || persisted->stage_completed ||
        persisted->activation_selected) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native stage cleanup changed before durable completion");
    }
    InsertNamedApplyStage(database, *stage);
    InsertNamedApplyDiscard(database, *stage);
    completion.Commit();
    stage->stage_completed = true;
    stage->discarded = true;
    return CapturedStageFailureResult(
        *stage, ProjectionApplyStageResultState::kRejected,
        NativeTransitionEvidence(*stage,
                                 ProjectionTransitionEvidenceState::kAborted,
                                 std::move(aborted_rows)),
        stage_error);
  }

  if (!stage->stage_completed) {
    Transaction completion(database, "BEGIN IMMEDIATE");
    const auto current =
        ReadCompleteDurableState(database, impl_->fixed_query_);
    const auto *persisted =
        current.FindNamedApply(stage->apply_id, stage->apply_generation);
    if (persisted == nullptr || persisted->stage_completed ||
        persisted->activation_selected || persisted->discarded) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native stage capture changed before completion commit");
    }
    InsertNamedApplyStage(database, *stage);
    completion.Commit();
    stage->stage_completed = true;
  }
  return StagedResult(*stage,
                      NativeTransitionEvidence(
                          *stage, ProjectionTransitionEvidenceState::kStaged,
                          std::move(staged_rows)));
}

ProjectionApplyActivationResultDTO
CanonicalEmptySceneOwner::ActivateStagedProjection(
    const ProjectionApplyActivationDTO &request) {
  if (!request.valid()) {
    throw std::invalid_argument("named Apply activation request is invalid");
  }
  ValidateBoundedIdentity(request.apply_id.value(), "Apply ID");
  static_cast<void>(
      CheckedSafeCounter(request.apply_generation.value(), "Apply generation"));
  static_cast<void>(CheckedSafeCounter(
      request.projection_stage_generation.value(), "stage generation"));
  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto *database = impl_->database();
  auto state = impl_->ReadStateTransaction();
  const auto *stored =
      state.FindNamedApply(request.apply_id, request.apply_generation);
  if (stored == nullptr) {
    const auto access = AccessStatus(state);
    const bool clean_absence = state.OpenNamedApply() == nullptr &&
                               access.active_projection.has_value() &&
                               *access.active_projection ==
                                   request.expected_active_projection_revision;
    return FailedActivationResult(
        request,
        clean_absence
            ? ProjectionApplyActivationResultState::kStale
            : ProjectionApplyActivationResultState::kFailedProjectionUncertain,
        clean_absence ? "named_apply_stage_absent"
                      : "named_apply_stage_identity_uncertain",
        clean_absence
            ? "the exact named Apply stage is durably absent"
            : "another or contradictory scene transition prevents proving "
              "the requested stage absent");
  }
  if (stored->stage_generation != request.projection_stage_generation ||
      stored->predecessor != request.expected_active_projection_revision ||
      stored->candidate != request.candidate_projection_revision) {
    return FailedActivationResult(
        request,
        ProjectionApplyActivationResultState::kFailedProjectionUncertain,
        "named_apply_activation_identity_mismatch",
        "the activation request does not match the exact durable stage");
  }
  NamedApplyStageState stage = *stored;

  if (stage.native_transition_rows.empty()) {
    if (stage.discarded) {
      return FailedActivationResult(
          request, ProjectionApplyActivationResultState::kStale,
          "named_apply_stage_discarded",
          "the exact named Apply stage was durably discarded");
    }
    if (!stage.stage_completed) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "zero-native stage cannot have an incomplete native capture");
    }
    if (stage.activated) {
      return ActivatedResult(stage);
    }
    Transaction transaction(database, "BEGIN IMMEDIATE");
    state = ReadCompleteDurableState(database, impl_->fixed_query_);
    stored = state.FindNamedApply(request.apply_id, request.apply_generation);
    if (stored == nullptr || stored->discarded || stored->activated ||
        !stored->stage_completed ||
        stored->stage_generation != request.projection_stage_generation ||
        stored->candidate != request.candidate_projection_revision) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "zero-native named Apply changed before activation commit");
    }
    InsertNamedApplyActivation(database, *stored);
    state = ReadCompleteDurableState(database, impl_->fixed_query_);
    stored = state.FindNamedApply(request.apply_id, request.apply_generation);
    if (stored == nullptr || !stored->activated || stored->discarded ||
        AccessStatus(state).scene_writes_open ||
        AccessStatus(state).projection_leases_open) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "named Apply activation did not persist exactly under quiesce");
    }
    stage = *stored;
    transaction.Commit();
    return ActivatedResult(stage);
  }

  std::vector<NativeTransitionParticipant> participants;
  try {
    participants = ObserveNativeTransitionParticipants(
        stage.native_transition_rows, impl_->projection_resolver_);
  } catch (const std::exception &error) {
    const ErrorDTO failure{
        "native_projection_set_unavailable", error.what(), {}};
    return FailedActivationResult(
        stage, ProjectionApplyActivationResultState::kFailedProjectionUncertain,
        UnavailableNativeTransitionEvidence(stage, failure), failure.code,
        failure.message);
  } catch (...) {
    const ErrorDTO failure{
        "native_projection_set_unavailable",
        "native projection participant observation threw a non-standard "
        "exception",
        {}};
    return FailedActivationResult(
        stage, ProjectionApplyActivationResultState::kFailedProjectionUncertain,
        UnavailableNativeTransitionEvidence(stage, failure), failure.code,
        failure.message);
  }

  if (stage.discarded) {
    try {
      return FailedActivationResult(
          stage, ProjectionApplyActivationResultState::kStale,
          ObserveNativeTransitionEvidence(
              stage, participants, ProjectionTransitionEvidenceState::kAborted),
          "named_apply_stage_discarded",
          "the exact named Apply stage was durably discarded");
    } catch (const std::exception &error) {
      const ErrorDTO failure{
          "native_projection_abort_status_uncertain", error.what(), {}};
      return FailedActivationResult(
          stage,
          ProjectionApplyActivationResultState::kFailedProjectionUncertain,
          UncertainNativeTransitionEvidence(stage, participants, failure),
          failure.code, failure.message);
    }
  }
  if (!stage.stage_completed) {
    const ErrorDTO failure{
        "native_projection_stage_incomplete",
        "the frozen native stage has not reached union-complete durable "
        "staging",
        {}};
    return FailedActivationResult(
        stage, ProjectionApplyActivationResultState::kFailedProjectionUncertain,
        UncertainNativeTransitionEvidence(stage, participants, failure),
        failure.code, failure.message);
  }
  if (stage.activated) {
    try {
      return ActivatedResult(
          stage, ObserveNativeTransitionEvidence(
                     stage, participants,
                     ProjectionTransitionEvidenceState::kActivatedAndRetired));
    } catch (const std::exception &error) {
      const ErrorDTO failure{
          "native_projection_activation_status_uncertain", error.what(), {}};
      return FailedActivationResult(
          stage,
          ProjectionApplyActivationResultState::kFailedProjectionUncertain,
          UncertainNativeTransitionEvidence(stage, participants, failure),
          failure.code, failure.message);
    }
  }

  if (!stage.activation_selected) {
    try {
      static_cast<void>(ObserveNativeTransitionEvidence(
          stage, participants, ProjectionTransitionEvidenceState::kStaged));
    } catch (const std::exception &error) {
      const ErrorDTO failure{
          "native_projection_stage_status_uncertain", error.what(), {}};
      return FailedActivationResult(
          stage,
          ProjectionApplyActivationResultState::kFailedProjectionUncertain,
          UncertainNativeTransitionEvidence(stage, participants, failure),
          failure.code, failure.message);
    }
    Transaction selection(database, "BEGIN IMMEDIATE");
    const auto current =
        ReadCompleteDurableState(database, impl_->fixed_query_);
    const auto *persisted =
        current.FindNamedApply(stage.apply_id, stage.apply_generation);
    if (persisted == nullptr || !persisted->stage_completed ||
        persisted->discarded || persisted->activated ||
        persisted->activation_selected ||
        persisted->native_transition_rows != stage.native_transition_rows ||
        persisted->candidate != stage.candidate) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native stage changed before durable activation selection");
    }
    InsertNativeActivationSelection(database, *persisted);
    selection.Commit();
    stage.activation_selected = true;
  }

  const auto uncertain_result =
      [&](std::vector<core::ProjectionTransitionRowEvidenceDTO> rows,
          const std::size_t next_row, const ErrorDTO &failure) {
        rows.reserve(participants.size());
        for (std::size_t index = next_row; index < participants.size();
             ++index) {
          rows.push_back(UncertainTransitionRowEvidence(
              participants[index].transition_row,
              participants[index].observed_health, failure));
        }
        return FailedActivationResult(
            stage,
            ProjectionApplyActivationResultState::kFailedProjectionUncertain,
            NativeTransitionEvidence(
                stage, ProjectionTransitionEvidenceState::kUncertain,
                std::move(rows), failure),
            failure.code, failure.message);
      };

  std::vector<core::ProjectionTransitionRowEvidenceDTO> activated_rows;
  activated_rows.reserve(participants.size());
  for (std::size_t index = 0U; index < participants.size(); ++index) {
    const auto &participant = participants[index];
    try {
      if (participant.transition_row.disposition ==
          core::ProjectionTransitionDisposition::kRemoved) {
        auto status = ReadNativeRetireStatus(stage, participant);
        if (status.state == core::NativeProjectionRetireStatusState::kPending) {
          const core::NativeProjectionRetireDTO retire{
              stage.apply_id,         stage.apply_generation,
              stage.stage_generation, participant.transition_row,
              stage.predecessor,      stage.candidate,
          };
          static_cast<void>(participant.projection->RetireProjection(retire));
          status = ReadNativeRetireStatus(stage, participant);
        }
        activated_rows.push_back(
            RemovedRetiredRowEvidence(participant, status));
      } else {
        auto status = ReadNativeStageStatus(stage, participant);
        if (status.state == core::NativeProjectionStatusState::kStaged) {
          const core::NativeProjectionActivationDTO activation{
              stage.apply_id,         stage.apply_generation,
              stage.stage_generation, participant.transition_row,
              stage.predecessor,      stage.candidate,
          };
          static_cast<void>(
              participant.projection->ActivateStagedProjection(activation));
          status = ReadNativeStageStatus(stage, participant);
        }
        activated_rows.push_back(
            CandidateActiveRowEvidence(stage, participant, status));
      }
    } catch (const std::exception &error) {
      const ErrorDTO failure{
          "native_projection_activation_uncertain", error.what(), {}};
      activated_rows.push_back(UncertainTransitionRowEvidence(
          participant.transition_row, participant.observed_health, failure));
      return uncertain_result(std::move(activated_rows), index + 1U, failure);
    } catch (...) {
      const ErrorDTO failure{
          "native_projection_activation_uncertain",
          "native projection activation or retirement threw a non-standard "
          "exception",
          {}};
      activated_rows.push_back(UncertainTransitionRowEvidence(
          participant.transition_row, participant.observed_health, failure));
      return uncertain_result(std::move(activated_rows), index + 1U, failure);
    }
  }

  auto evidence = NativeTransitionEvidence(
      stage, ProjectionTransitionEvidenceState::kActivatedAndRetired,
      std::move(activated_rows));
  Transaction completion(database, "BEGIN IMMEDIATE");
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  stored = state.FindNamedApply(stage.apply_id, stage.apply_generation);
  if (stored == nullptr || !stored->stage_completed ||
      !stored->activation_selected || stored->discarded ||
      stored->finalization_receipt.has_value() ||
      stored->native_transition_rows != stage.native_transition_rows) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native activation selection changed before completion commit");
  }
  if (!stored->activated) {
    InsertNamedApplyActivation(database, *stored);
  }
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  stored = state.FindNamedApply(stage.apply_id, stage.apply_generation);
  if (stored == nullptr || !stored->activated || !stored->activation_selected ||
      stored->discarded || AccessStatus(state).scene_writes_open ||
      AccessStatus(state).projection_leases_open) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native activation did not persist exactly under quiesce");
  }
  stage = *stored;
  completion.Commit();
  return ActivatedResult(stage, std::move(evidence));
}

ProjectionApplyDiscardResultDTO
CanonicalEmptySceneOwner::DiscardStagedProjection(
    const ProjectionApplyDiscardDTO &request) {
  if (!request.valid()) {
    throw std::invalid_argument("named Apply discard request is invalid");
  }
  ValidateBoundedIdentity(request.apply_id.value(), "Apply ID");
  static_cast<void>(
      CheckedSafeCounter(request.apply_generation.value(), "Apply generation"));
  static_cast<void>(CheckedSafeCounter(
      request.projection_stage_generation.value(), "stage generation"));
  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto *database = impl_->database();
  auto state = impl_->ReadStateTransaction();
  const auto *stored =
      state.FindNamedApply(request.apply_id, request.apply_generation);
  if (stored == nullptr) {
    const auto access = AccessStatus(state);
    if (state.OpenNamedApply() == nullptr &&
        access.active_projection.has_value() &&
        *access.active_projection ==
            request.expected_predecessor_projection_revision) {
      return DiscardedResult(StageIdentityFrom(request),
                             ProjectionApplyDiscardResultState::kAlreadyAbsent);
    }
    return FailedDiscardResult(
        request, "named_apply_discard_identity_uncertain",
        "another or contradictory transition prevents proving the exact "
        "candidate stage absent");
  }
  if (stored->stage_generation != request.projection_stage_generation ||
      stored->predecessor != request.expected_predecessor_projection_revision ||
      stored->candidate != request.candidate_projection_revision) {
    return FailedDiscardResult(
        request, "named_apply_discard_identity_mismatch",
        "the discard request does not match the exact durable stage");
  }
  NamedApplyStageState stage = *stored;

  if (stage.native_transition_rows.empty()) {
    if (stage.activated || stage.finalization_receipt.has_value()) {
      return FailedDiscardResult(
          request, "named_apply_discard_forbidden_after_activation",
          "the exact transition has activated and can only recover forward");
    }
    if (stage.discarded) {
      return DiscardedResult(stage,
                             ProjectionApplyDiscardResultState::kDiscarded);
    }
    Transaction transaction(database, "BEGIN IMMEDIATE");
    state = ReadCompleteDurableState(database, impl_->fixed_query_);
    stored = state.FindNamedApply(request.apply_id, request.apply_generation);
    if (stored == nullptr || stored->activated || stored->discarded ||
        stored->finalization_receipt.has_value()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "zero-native stage changed before discard commit");
    }
    InsertNamedApplyDiscard(database, *stored);
    state = ReadCompleteDurableState(database, impl_->fixed_query_);
    stored = state.FindNamedApply(request.apply_id, request.apply_generation);
    const auto access = AccessStatus(state);
    if (stored == nullptr || !stored->discarded || stored->activated ||
        stored->finalization_receipt.has_value() || !access.scene_writes_open ||
        !access.projection_leases_open ||
        access.active_projection !=
            std::optional<ProjectionRevisionDTO>{stored->predecessor}) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "named Apply discard did not restore the exact predecessor");
    }
    stage = *stored;
    transaction.Commit();
    return DiscardedResult(stage,
                           ProjectionApplyDiscardResultState::kDiscarded);
  }

  std::vector<NativeTransitionParticipant> participants;
  try {
    participants = ObserveNativeTransitionParticipants(
        stage.native_transition_rows, impl_->projection_resolver_);
  } catch (const std::exception &error) {
    const ErrorDTO failure{
        "native_projection_set_unavailable", error.what(), {}};
    return FailedDiscardResult(
        stage, UnavailableNativeTransitionEvidence(stage, failure),
        failure.code, failure.message);
  } catch (...) {
    const ErrorDTO failure{
        "native_projection_set_unavailable",
        "native projection participant observation threw a non-standard "
        "exception",
        {}};
    return FailedDiscardResult(
        stage, UnavailableNativeTransitionEvidence(stage, failure),
        failure.code, failure.message);
  }

  if (stage.activation_selected || stage.activated ||
      stage.finalization_receipt.has_value()) {
    const ErrorDTO failure{
        "named_apply_discard_forbidden_after_activation",
        "the exact transition has selected activation and can only recover "
        "forward",
        {}};
    try {
      auto observed = ObserveNativeTransitionEvidence(
          stage, participants,
          stage.finalization_receipt.has_value()
              ? ProjectionTransitionEvidenceState::kFinalized
              : ProjectionTransitionEvidenceState::kActivatedAndRetired);
      return FailedDiscardResult(
          stage,
          NativeTransitionEvidence(
              stage, ProjectionTransitionEvidenceState::kUncertain,
              std::move(observed.transition_rows), failure),
          failure.code, failure.message);
    } catch (const std::exception &) {
      return FailedDiscardResult(
          stage,
          UncertainNativeTransitionEvidence(stage, participants, failure),
          failure.code, failure.message);
    }
  }
  if (stage.discarded) {
    try {
      return DiscardedResult(stage,
                             ProjectionApplyDiscardResultState::kDiscarded,
                             ObserveNativeTransitionEvidence(
                                 stage, participants,
                                 ProjectionTransitionEvidenceState::kAborted));
    } catch (const std::exception &error) {
      const ErrorDTO failure{
          "native_projection_abort_status_uncertain", error.what(), {}};
      return FailedDiscardResult(
          stage,
          UncertainNativeTransitionEvidence(stage, participants, failure),
          failure.code, failure.message);
    }
  }

  const auto uncertain_result =
      [&](std::vector<core::ProjectionTransitionRowEvidenceDTO> rows,
          const std::size_t next_row, const ErrorDTO &failure) {
        rows.reserve(participants.size());
        for (std::size_t index = next_row; index < participants.size();
             ++index) {
          rows.push_back(UncertainTransitionRowEvidence(
              participants[index].transition_row,
              participants[index].observed_health, failure));
        }
        return FailedDiscardResult(
            stage,
            NativeTransitionEvidence(
                stage, ProjectionTransitionEvidenceState::kUncertain,
                std::move(rows), failure),
            failure.code, failure.message);
      };

  std::vector<core::ProjectionTransitionRowEvidenceDTO> aborted_rows;
  aborted_rows.reserve(participants.size());
  for (std::size_t index = 0U; index < participants.size(); ++index) {
    const auto &participant = participants[index];
    try {
      if (participant.transition_row.disposition ==
          core::ProjectionTransitionDisposition::kRemoved) {
        aborted_rows.push_back(PredecessorUnchangedRowEvidence(
            stage, participant, ReadNativeRetireStatus(stage, participant)));
      } else {
        auto status = ReadNativeStageStatus(stage, participant);
        if (status.state == core::NativeProjectionStatusState::kStaged) {
          const core::NativeProjectionDiscardDTO discard{
              stage.apply_id,         stage.apply_generation,
              stage.stage_generation, participant.transition_row,
              stage.candidate,
          };
          static_cast<void>(
              participant.projection->DiscardStagedProjection(discard));
          status = ReadNativeStageStatus(stage, participant);
        }
        aborted_rows.push_back(
            CandidateDiscardedRowEvidence(stage, participant, status));
      }
    } catch (const std::exception &error) {
      const ErrorDTO failure{
          "native_projection_discard_uncertain", error.what(), {}};
      aborted_rows.push_back(UncertainTransitionRowEvidence(
          participant.transition_row, participant.observed_health, failure));
      return uncertain_result(std::move(aborted_rows), index + 1U, failure);
    } catch (...) {
      const ErrorDTO failure{
          "native_projection_discard_uncertain",
          "native projection discard threw a non-standard exception",
          {}};
      aborted_rows.push_back(UncertainTransitionRowEvidence(
          participant.transition_row, participant.observed_health, failure));
      return uncertain_result(std::move(aborted_rows), index + 1U, failure);
    }
  }

  auto evidence = NativeTransitionEvidence(
      stage, ProjectionTransitionEvidenceState::kAborted,
      std::move(aborted_rows));
  Transaction transaction(database, "BEGIN IMMEDIATE");
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  stored = state.FindNamedApply(stage.apply_id, stage.apply_generation);
  if (stored == nullptr || stored->activation_selected || stored->activated ||
      stored->discarded || stored->finalization_receipt.has_value() ||
      stored->native_transition_rows != stage.native_transition_rows) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native stage changed before discard completion");
  }
  if (!stored->stage_completed) {
    InsertNamedApplyStage(database, *stored);
  }
  InsertNamedApplyDiscard(database, *stored);
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  stored = state.FindNamedApply(stage.apply_id, stage.apply_generation);
  const auto access = AccessStatus(state);
  if (stored == nullptr || !stored->discarded || stored->activated ||
      stored->activation_selected || stored->finalization_receipt.has_value() ||
      !access.scene_writes_open || !access.projection_leases_open ||
      access.active_projection !=
          std::optional<ProjectionRevisionDTO>{stored->predecessor}) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native named Apply discard did not restore the exact predecessor");
  }
  stage = *stored;
  transaction.Commit();
  return DiscardedResult(stage, ProjectionApplyDiscardResultState::kDiscarded,
                         std::move(evidence));
}

ProjectionApplyFinalizeResultDTO
CanonicalEmptySceneOwner::FinalizeProjectionTransition(
    const ProjectionApplyFinalizeDTO &request) {
  if (!request.valid()) {
    throw std::invalid_argument("named Apply Finalize request is invalid");
  }
  ValidateBoundedIdentity(request.apply_id.value(), "Apply ID");
  static_cast<void>(
      CheckedSafeCounter(request.apply_generation.value(), "Apply generation"));
  static_cast<void>(CheckedSafeCounter(
      request.projection_stage_generation.value(), "stage generation"));
  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto state = impl_->ReadStateTransaction();
  const auto *stored =
      state.FindNamedApply(request.apply_id, request.apply_generation);
  if (stored == nullptr) {
    throw CanonicalEmptySceneOwnerConflictError(
        "named Apply Finalize has no exact durable stage");
  }
  if (stored->stage_generation != request.projection_stage_generation ||
      stored->candidate != request.candidate_projection_revision) {
    throw CanonicalEmptySceneOwnerConflictError(
        "named Apply Finalize does not match the durable stage identity");
  }
  if (stored->discarded || !stored->activated) {
    throw CanonicalEmptySceneOwnerConflictError(
        "named Apply Finalize requires the exact activated transition");
  }
  NamedApplyStageState stage = *stored;
  if (stage.finalization_receipt.has_value() &&
      *stage.finalization_receipt != request.publication_receipt) {
    throw CanonicalEmptySceneOwnerConflictError(
        "named Apply Finalize replay carries a different receipt");
  }

  if (stage.native_transition_rows.empty()) {
    if (stage.finalization_receipt.has_value()) {
      return FinalizedResult(stage);
    }
    core::Result<ActiveBundlePublicationReceiptDTO> durable = [&] {
      try {
        return impl_->publication_reader_.ReadActiveBundlePublication(
            request.publication_receipt.publication_generation);
      } catch (const std::exception &error) {
        return core::Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
            "active_bundle_publication_read_uncertain", error.what(), {}});
      } catch (...) {
        return core::Result<ActiveBundlePublicationReceiptDTO>::Err(
            ErrorDTO{"active_bundle_publication_read_uncertain",
                     "publication reader threw a non-standard exception",
                     {}});
      }
    }();
    if (!durable.ok()) {
      const auto &read_error_code = durable.error().code;
      const bool definitively_not_current =
          read_error_code == "missing_reference" ||
          read_error_code == "stale_generation" ||
          read_error_code == "publication_generation_not_current";
      return FailedNamedFinalizeResult(
          stage, request,
          definitively_not_current
              ? ProjectionApplyFinalizeResultState::kStale
              : ProjectionApplyFinalizeResultState::kFailedProjectionUncertain,
          definitively_not_current ? "active_bundle_publication_not_current"
                                   : "active_bundle_publication_read_uncertain",
          definitively_not_current
              ? "the requested publication generation is not the current "
                "durable head"
              : "the current publication head could not be read "
                "authoritatively");
    }
    if (durable.value() != request.publication_receipt) {
      return FailedNamedFinalizeResult(
          stage, request, ProjectionApplyFinalizeResultState::kStale,
          "active_bundle_publication_receipt_mismatch",
          "the independently reread current receipt is not byte-equivalent");
    }

    auto *database = impl_->database();
    Transaction transaction(database, "BEGIN IMMEDIATE");
    state = ReadCompleteDurableState(database, impl_->fixed_query_);
    stored = state.FindNamedApply(request.apply_id, request.apply_generation);
    if (stored == nullptr || stored->discarded || !stored->activated ||
        stored->stage_generation != request.projection_stage_generation ||
        stored->candidate != request.candidate_projection_revision) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "named Apply transition changed before Finalize commit");
    }
    if (!stored->finalization_receipt.has_value()) {
      const auto owner_event_sequence = NextSceneOwnerEventSequence(database);
      const auto lineage_generation = stored->lineage_generation;
      InsertNamedApplyFinalization(database, request);
      InsertNamedApplyOwnerEvent(database, owner_event_sequence,
                                 lineage_generation);
      state = ReadCompleteDurableState(database, impl_->fixed_query_);
      stored = state.FindNamedApply(request.apply_id, request.apply_generation);
    }
    const auto access = AccessStatus(state);
    if (stored == nullptr || !stored->finalization_receipt.has_value() ||
        *stored->finalization_receipt != request.publication_receipt ||
        !access.scene_writes_open || !access.projection_leases_open ||
        access.active_projection !=
            std::optional<ProjectionRevisionDTO>{stored->candidate}) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "named Apply Finalize did not durably open the exact candidate");
    }
    stage = *stored;
    transaction.Commit();
    return FinalizedResult(stage);
  }

  if (!stage.activation_selected) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native named Apply activation lacks its durable selection");
  }

  std::vector<NativeTransitionParticipant> participants;
  try {
    participants = ObserveNativeTransitionParticipants(
        stage.native_transition_rows, impl_->projection_resolver_);
  } catch (const std::exception &error) {
    const ErrorDTO failure{
        "native_projection_set_unavailable", error.what(), {}};
    return FailedNamedFinalizeResult(
        stage, request,
        ProjectionApplyFinalizeResultState::kFailedProjectionUncertain,
        UnavailableNativeTransitionEvidence(stage, failure),
        UnavailableCandidateAggregateHealth(stage, failure), failure.code,
        failure.message);
  } catch (...) {
    const ErrorDTO failure{
        "native_projection_set_unavailable",
        "native projection participant observation threw a non-standard "
        "exception",
        {}};
    return FailedNamedFinalizeResult(
        stage, request,
        ProjectionApplyFinalizeResultState::kFailedProjectionUncertain,
        UnavailableNativeTransitionEvidence(stage, failure),
        UnavailableCandidateAggregateHealth(stage, failure), failure.code,
        failure.message);
  }

  std::optional<ProjectionTransitionEvidenceDTO> activated_evidence;
  std::optional<ProjectionAggregateHealthDTO> candidate_health;
  try {
    activated_evidence.emplace(ObserveNativeTransitionEvidence(
        stage, participants,
        ProjectionTransitionEvidenceState::kActivatedAndRetired));
    candidate_health.emplace(
        CandidateAggregateHealth(stage, activated_evidence->transition_rows));
  } catch (const std::exception &error) {
    const ErrorDTO failure{
        "native_projection_finalize_status_uncertain", error.what(), {}};
    return FailedNamedFinalizeResult(
        stage, request,
        ProjectionApplyFinalizeResultState::kFailedProjectionUncertain,
        UncertainNativeTransitionEvidence(stage, participants, failure),
        UnavailableCandidateAggregateHealth(stage, failure), failure.code,
        failure.message);
  }

  core::Result<ActiveBundlePublicationReceiptDTO> durable = [&] {
    try {
      return impl_->publication_reader_.ReadActiveBundlePublication(
          request.publication_receipt.publication_generation);
    } catch (const std::exception &error) {
      return core::Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "active_bundle_publication_read_uncertain", error.what(), {}});
    } catch (...) {
      return core::Result<ActiveBundlePublicationReceiptDTO>::Err(
          ErrorDTO{"active_bundle_publication_read_uncertain",
                   "publication reader threw a non-standard exception",
                   {}});
    }
  }();
  if (!durable.ok()) {
    const auto &read_error_code = durable.error().code;
    const bool definitively_not_current =
        read_error_code == "missing_reference" ||
        read_error_code == "stale_generation" ||
        read_error_code == "publication_generation_not_current";
    if (definitively_not_current) {
      return FailedNamedFinalizeResult(
          stage, request, ProjectionApplyFinalizeResultState::kStale,
          std::move(*activated_evidence), std::move(*candidate_health),
          "active_bundle_publication_not_current",
          "the requested publication generation is not the current durable "
          "head");
    }
    const ErrorDTO failure{"active_bundle_publication_read_uncertain",
                           "the current publication head could not be read "
                           "authoritatively",
                           {}};
    auto rows = std::move(activated_evidence->transition_rows);
    return FailedNamedFinalizeResult(
        stage, request,
        ProjectionApplyFinalizeResultState::kFailedProjectionUncertain,
        NativeTransitionEvidence(stage,
                                 ProjectionTransitionEvidenceState::kUncertain,
                                 std::move(rows), failure),
        UnavailableCandidateAggregateHealth(stage, failure), failure.code,
        failure.message);
  }
  if (durable.value() != request.publication_receipt) {
    return FailedNamedFinalizeResult(
        stage, request, ProjectionApplyFinalizeResultState::kStale,
        std::move(*activated_evidence), std::move(*candidate_health),
        "active_bundle_publication_receipt_mismatch",
        "the independently reread current receipt is not byte-equivalent");
  }

  auto finalized_rows = activated_evidence->transition_rows;
  auto *database = impl_->database();
  Transaction transaction(database, "BEGIN IMMEDIATE");
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  stored = state.FindNamedApply(request.apply_id, request.apply_generation);
  if (stored == nullptr || stored->discarded || !stored->activated ||
      !stored->activation_selected ||
      stored->stage_generation != request.projection_stage_generation ||
      stored->candidate != request.candidate_projection_revision ||
      stored->native_transition_rows != stage.native_transition_rows) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native named Apply transition changed before Finalize commit");
  }
  if (!stored->finalization_receipt.has_value()) {
    const auto owner_event_sequence = NextSceneOwnerEventSequence(database);
    const auto lineage_generation = stored->lineage_generation;
    InsertNamedApplyFinalization(database, request);
    InsertNamedApplyOwnerEvent(database, owner_event_sequence,
                               lineage_generation);
    state = ReadCompleteDurableState(database, impl_->fixed_query_);
    stored = state.FindNamedApply(request.apply_id, request.apply_generation);
  }
  const auto access = AccessStatus(state);
  if (stored == nullptr || !stored->finalization_receipt.has_value() ||
      *stored->finalization_receipt != request.publication_receipt ||
      !access.scene_writes_open || !access.projection_leases_open ||
      access.active_projection !=
          std::optional<ProjectionRevisionDTO>{stored->candidate}) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native named Apply Finalize did not durably open the exact candidate");
  }
  stage = *stored;
  transaction.Commit();
  return FinalizedResult(
      stage,
      NativeTransitionEvidence(stage,
                               ProjectionTransitionEvidenceState::kFinalized,
                               std::move(finalized_rows)),
      std::move(*candidate_health));
}

ProjectionApplyStatusDTO CanonicalEmptySceneOwner::ProjectionApplyStatus(
    const ProjectionApplyStatusQueryDTO &query) const {
  if (!query.valid()) {
    throw std::invalid_argument("named Apply status query is invalid");
  }
  ValidateBoundedIdentity(query.apply_id.value(), "Apply ID");
  static_cast<void>(
      CheckedSafeCounter(query.apply_generation.value(), "Apply generation"));
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadStateTransaction();
  const auto *staged =
      state.FindNamedApply(query.apply_id, query.apply_generation);
  if (staged == nullptr) {
    ProjectionApplyStatusDTO absent{
        query.apply_id, query.apply_generation,
        std::nullopt,   std::nullopt,
        std::nullopt,   ProjectionApplyStatusState::kAbsent,
        std::nullopt,   std::nullopt,
        std::nullopt,   std::nullopt,
    };
    if (!absent.presence_valid()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "cannot represent absent named Apply status");
    }
    return absent;
  }
  const NamedApplyStageState stage = *staged;
  const auto status_state =
      stage.finalization_receipt.has_value()
          ? ProjectionApplyStatusState::kFinalized
      : stage.discarded ? ProjectionApplyStatusState::kDiscarded
      : stage.activated ? ProjectionApplyStatusState::kActivated
                        : ProjectionApplyStatusState::kStaged;
  const auto evidence_state =
      stage.finalization_receipt.has_value()
          ? ProjectionTransitionEvidenceState::kFinalized
      : stage.discarded ? ProjectionTransitionEvidenceState::kAborted
      : stage.activated
          ? ProjectionTransitionEvidenceState::kActivatedAndRetired
          : ProjectionTransitionEvidenceState::kStaged;

  if (stage.native_transition_rows.empty()) {
    ProjectionApplyStatusDTO status{
        stage.apply_id,
        stage.apply_generation,
        stage.predecessor,
        stage.stage_generation,
        stage.candidate,
        status_state,
        ZeroNativeTransitionEvidence(stage, evidence_state),
        stage.finalization_receipt,
        stage.finalization_receipt.has_value()
            ? std::optional<ProjectionAggregateHealthDTO>(
                  ZeroNativeNamedApplyHealth(stage))
            : std::nullopt,
        std::nullopt,
    };
    if (!status.presence_valid()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "durable zero-native named Apply stage cannot form a valid status");
    }
    return status;
  }

  const auto failed_status = [&](ProjectionTransitionEvidenceDTO evidence,
                                 ErrorDTO error) {
    ProjectionApplyStatusDTO status{
        stage.apply_id,
        stage.apply_generation,
        stage.predecessor,
        stage.stage_generation,
        stage.candidate,
        ProjectionApplyStatusState::kFailedProjectionUncertain,
        std::move(evidence),
        stage.finalization_receipt,
        std::nullopt,
        std::move(error),
    };
    if (!status.presence_valid()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native named Apply uncertainty cannot form a valid status");
    }
    return status;
  };

  std::vector<NativeTransitionParticipant> participants;
  try {
    participants = ObserveNativeTransitionParticipants(
        stage.native_transition_rows, impl_->projection_resolver_);
  } catch (const std::exception &error) {
    const ErrorDTO failure{
        "native_projection_set_unavailable", error.what(), {}};
    return failed_status(UnavailableNativeTransitionEvidence(stage, failure),
                         failure);
  } catch (...) {
    const ErrorDTO failure{
        "native_projection_set_unavailable",
        "native projection participant observation threw a non-standard "
        "exception",
        {}};
    return failed_status(UnavailableNativeTransitionEvidence(stage, failure),
                         failure);
  }

  if (!stage.stage_completed ||
      (stage.activation_selected && !stage.activated)) {
    const ErrorDTO failure{
        stage.activation_selected ? "native_projection_activation_incomplete"
                                  : "native_projection_stage_incomplete",
        stage.activation_selected
            ? "forward-only native activation has not reached a durable "
              "union-complete result"
            : "native candidate staging has not reached a durable "
              "union-complete result",
        {}};
    return failed_status(
        UncertainNativeTransitionEvidence(stage, participants, failure),
        failure);
  }

  try {
    auto evidence =
        ObserveNativeTransitionEvidence(stage, participants, evidence_state);
    std::optional<ProjectionAggregateHealthDTO> candidate_health;
    if (stage.finalization_receipt.has_value()) {
      candidate_health =
          CandidateAggregateHealth(stage, evidence.transition_rows);
    }
    ProjectionApplyStatusDTO status{
        stage.apply_id,
        stage.apply_generation,
        stage.predecessor,
        stage.stage_generation,
        stage.candidate,
        status_state,
        std::move(evidence),
        stage.finalization_receipt,
        std::move(candidate_health),
        std::nullopt,
    };
    if (!status.presence_valid()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native named Apply stage cannot form a valid status");
    }
    return status;
  } catch (const std::exception &error) {
    const ErrorDTO failure{
        "native_projection_status_uncertain", error.what(), {}};
    return failed_status(
        UncertainNativeTransitionEvidence(stage, participants, failure),
        failure);
  } catch (...) {
    const ErrorDTO failure{
        "native_projection_status_uncertain",
        "native projection status query threw a non-standard exception",
        {}};
    return failed_status(
        UncertainNativeTransitionEvidence(stage, participants, failure),
        failure);
  }
}

ActiveProjectionRuntimeRecoveryResultDTO
CanonicalEmptySceneOwner::ReconcileActiveProjectionRuntimes(
    const ActiveProjectionRuntimeSetDTO &runtime_set) {
  if (!runtime_set.valid() || runtime_set.native_runtime_bindings.size() >
                                  kMaximumNativeProjectionTransitionRows) {
    throw std::invalid_argument(
        "active native projection runtime set is invalid");
  }
  for (const auto &binding : runtime_set.native_runtime_bindings) {
    ValidateBoundedIdentity(binding.provider_binding_id.value(),
                            "provider binding ID");
    ValidateBoundedIdentity(binding.provider_binding_revision.value(),
                            "provider binding revision");
    ValidateBoundedIdentity(binding.runtime_unit_id.value(), "runtime unit ID");
    ValidateBoundedIdentity(binding.runtime_epoch.value(), "runtime epoch");
  }

  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto *database = impl_->database();
  auto state = impl_->ReadStateTransaction();
  if (!state.active_projection.has_value() ||
      !state.active_scene_snapshot.has_value() ||
      *state.active_projection != runtime_set.active_projection_revision ||
      state.OpenNamedApply() != nullptr || state.unresolved_scene_transaction ||
      (state.projection_desynchronized &&
       !state.latest_uncertain_transaction_sequence.has_value())) {
    throw CanonicalEmptySceneOwnerConflictError(
        "native runtime recovery requires the exact stable active scene");
  }

  const auto current_bindings = ActiveNativeRuntimeBindings(state);
  const auto *latest_recovery = state.LatestActiveRuntimeRecovery();
  const bool pending_recovery = latest_recovery != nullptr &&
                                !latest_recovery->completion_health.has_value();
  if (current_bindings == runtime_set.native_runtime_bindings &&
      !pending_recovery && !state.projection_desynchronized) {
    ActiveProjectionRuntimeRecoveryResultDTO result{
        runtime_set,
        ActiveProjectionRuntimeRecoveryState::kCurrent,
        latest_recovery == nullptr
            ? std::nullopt
            : std::optional<
                  NativeProjectionRecoveryGeneration>{latest_recovery
                                                          ->recovery_generation},
    };
    if (!result.valid()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "cannot represent current native runtime recovery result");
    }
    return result;
  }

  const auto *active_stage = state.ActiveStructuralStage();
  if (active_stage == nullptr || active_stage->native_transition_rows.empty() ||
      runtime_set.native_runtime_bindings.empty() ||
      !SameProjectionRequirementSet(*active_stage,
                                    runtime_set.native_runtime_bindings) ||
      impl_->retained_bundle_source_ == nullptr ||
      impl_->projection_resolver_ == nullptr) {
    throw CanonicalEmptySceneOwnerConflictError(
        "active deployment has no exact recoverable native projection set");
  }

  std::shared_ptr<const deployment::RetainedBundle> retained_bundle;
  try {
    retained_bundle = impl_->retained_bundle_source_->get_verified(
        runtime_set.active_projection_revision.retained_bundle_id);
  } catch (const std::exception &error) {
    throw CanonicalEmptySceneOwnerConflictError(
        "active retained bundle reload failed during native recovery: " +
        std::string(error.what()));
  } catch (...) {
    throw CanonicalEmptySceneOwnerConflictError(
        "active retained bundle reload failed during native recovery");
  }
  if (retained_bundle == nullptr ||
      !deployment::verify_retained_bundle_identity(*retained_bundle) ||
      retained_bundle->retained_bundle_id() !=
          runtime_set.active_projection_revision.retained_bundle_id ||
      retained_bundle->value().cell_graph.graph_revision !=
          runtime_set.active_projection_revision.graph_revision ||
      retained_bundle->value().cell_graph.cell_id !=
          impl_->fixed_query_.cell_id ||
      NativeSceneProviderBindings(retained_bundle->value()).size() !=
          runtime_set.native_runtime_bindings.size()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "active retained bundle differs from the native recovery target");
  }

  NativeProjectionRecoveryGeneration recovery_generation(0U);
  if (pending_recovery &&
      latest_recovery->target == runtime_set.active_projection_revision &&
      latest_recovery->bindings == runtime_set.native_runtime_bindings) {
    recovery_generation = latest_recovery->recovery_generation;
  } else {
    const auto history = ReadBoundedHistoryState(database);
    if (history.native_recovery_generation_watermark >= kMaximumSafeIntegral) {
      throw CanonicalEmptySceneOwnerLimitError(
          "native runtime recovery generation is exhausted");
    }
    recovery_generation = NativeProjectionRecoveryGeneration(
        history.native_recovery_generation_watermark + 1U);
    Transaction intent(database, "BEGIN IMMEDIATE");
    const auto current =
        ReadCompleteDurableState(database, impl_->fixed_query_);
    if (!current.active_projection.has_value() ||
        !current.active_scene_snapshot.has_value() ||
        *current.active_projection != runtime_set.active_projection_revision ||
        *current.active_scene_snapshot != *state.active_scene_snapshot ||
        current.OpenNamedApply() != nullptr ||
        current.native_runtime_recoveries.size() !=
            state.native_runtime_recoveries.size()) {
      throw CanonicalEmptySceneOwnerConflictError(
          "active scene changed before native recovery intent commit");
    }
    CompactNativeRuntimeRecoveryHistory(database, current, recovery_generation);
    InsertNativeRuntimeRecoveryIntent(
        database, recovery_generation, active_stage->lineage_generation,
        runtime_set,
        state.projection_desynchronized
            ? state.latest_uncertain_transaction_sequence
            : std::nullopt);
    CloseNativeRuntimeRecoveryCompaction(database);
    intent.Commit();
    state = impl_->ReadStateTransaction();
    latest_recovery = state.LatestActiveRuntimeRecovery();
    if (latest_recovery == nullptr ||
        latest_recovery->recovery_generation != recovery_generation ||
        latest_recovery->target != runtime_set.active_projection_revision ||
        latest_recovery->bindings != runtime_set.native_runtime_bindings ||
        latest_recovery->completion_health.has_value() ||
        AccessStatus(state).scene_writes_open ||
        AccessStatus(state).projection_leases_open) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native runtime recovery intent did not close the scene gates");
    }
  }

  const LogicalSceneSnapshotRoot snapshot(
      std::make_shared<const LogicalSceneSnapshotDTO>(
          *state.active_scene_snapshot));
  std::vector<core::ProjectionHealthDTO> recovered_health;
  recovered_health.reserve(runtime_set.native_runtime_bindings.size());
  for (const auto &binding : runtime_set.native_runtime_bindings) {
    auto required =
        ResolveRequiredProjection(binding, impl_->projection_resolver_);
    auto health = std::move(required.preflight_health);
    if (!health.healthy_for_target(runtime_set.active_projection_revision)) {
      const core::NativeProjectionRebuildDTO rebuild{
          binding,
          runtime_set.active_projection_revision,
          retained_bundle->value().compiled_products.planning_collision_model,
          snapshot.shared(),
      };
      auto result = required.projection->RebuildProjection(rebuild);
      if (!result.presence_valid() || result.transaction_sequence.has_value() ||
          result.proposed_projection_revision !=
              runtime_set.active_projection_revision ||
          result.result != core::ProjectionCommitResultStateDTO::kCommitted ||
          result.acknowledged_projection_revision !=
              std::optional<ProjectionRevisionDTO>{
                  runtime_set.active_projection_revision} ||
          result.resulting_health.native_runtime_binding != binding ||
          !result.resulting_health.healthy_for_target(
              runtime_set.active_projection_revision)) {
        throw CanonicalEmptySceneOwnerConflictError(
            "replacement native projection did not acknowledge the exact "
            "active snapshot rebuild");
      }
      health = std::move(result.resulting_health);
    }
    if (health.native_runtime_binding != binding ||
        health.source_sequence == 0U ||
        !health.native_projection_generation.has_value() ||
        !health.projection_content_digest.has_value() ||
        !health.healthy_for_target(runtime_set.active_projection_revision)) {
      throw CanonicalEmptySceneOwnerConflictError(
          "replacement native projection lacks complete healthy rebuild "
          "evidence");
    }
    recovered_health.push_back(std::move(health));
  }

  Transaction completion(database, "BEGIN IMMEDIATE");
  const auto before_completion =
      ReadCompleteDurableState(database, impl_->fixed_query_);
  const auto *pending = before_completion.LatestActiveRuntimeRecovery();
  if (!before_completion.active_projection.has_value() ||
      !before_completion.active_scene_snapshot.has_value() ||
      *before_completion.active_projection !=
          runtime_set.active_projection_revision ||
      *before_completion.active_scene_snapshot !=
          *state.active_scene_snapshot ||
      pending == nullptr ||
      pending->recovery_generation != recovery_generation ||
      pending->target != runtime_set.active_projection_revision ||
      pending->bindings != runtime_set.native_runtime_bindings ||
      pending->completion_health.has_value()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "active scene changed before native recovery completion commit");
  }
  InsertNativeRuntimeRecoveryCompletion(database, recovery_generation,
                                        runtime_set.active_projection_revision,
                                        recovered_health);
  if (pending->recovered_transaction_sequence.has_value()) {
    Statement recovered(
        database,
        "UPDATE logical_scene_history_state SET "
        "recovered_through_transaction_sequence=? WHERE singleton=1 AND "
        "compaction_in_progress=0 AND "
        "recovered_through_transaction_sequence<=?");
    recovered.Bind(1,
                   CheckedSafeCounter(*pending->recovered_transaction_sequence,
                                      "recovered transaction sequence"));
    recovered.Bind(2,
                   CheckedSafeCounter(*pending->recovered_transaction_sequence,
                                      "recovered transaction sequence"));
    if (recovered.Step() || sqlite3_changes(database) != 1) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "native recovery could not advance the transaction recovery "
          "watermark");
    }
  }
  completion.Commit();

  const auto completed = impl_->ReadStateTransaction();
  const auto *stored = completed.LatestActiveRuntimeRecovery();
  const auto access = AccessStatus(completed);
  if (stored == nullptr || stored->recovery_generation != recovery_generation ||
      !stored->completion_health.has_value() ||
      ActiveNativeRuntimeBindings(completed) !=
          runtime_set.native_runtime_bindings ||
      !access.scene_writes_open || !access.projection_leases_open) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "native runtime recovery did not select the exact rebuilt set");
  }
  ActiveProjectionRuntimeRecoveryResultDTO result{
      runtime_set,
      ActiveProjectionRuntimeRecoveryState::kRecovered,
      recovery_generation,
  };
  if (!result.valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent completed native runtime recovery result");
  }
  return result;
}

ProjectionAggregateHealthDTO
CanonicalEmptySceneOwner::ActiveProjectionHealth() const {
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadSceneCommitState().durable_state;
  if (!state.active_projection.has_value() ||
      !state.active_scene_snapshot.has_value() || !state.accepted.has_value()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "active native projection health requires finalized bootstrap");
  }
  core::RequiredNativeProjectionSetSourceDTO source =
      [&]() -> core::RequiredNativeProjectionSetSourceDTO {
    if (const auto *stage = state.ActiveStructuralStage(); stage != nullptr) {
      return NamedApplyProjectionSetSourceDTO{
          stage->apply_id, stage->apply_generation, stage->stage_generation,
          stage->candidate};
    }
    return BootstrapProjectionSetSourceDTO{state.accepted->bootstrap_id,
                                           state.accepted->bootstrap_generation,
                                           state.accepted->target};
  }();

  const auto bindings = ActiveNativeRuntimeBindings(state);
  std::vector<core::ProjectionHealthDTO> health;
  health.reserve(bindings.size());
  std::optional<ErrorDTO> aggregate_error;
  const auto access = AccessStatus(state);
  for (const auto &binding : bindings) {
    try {
      auto observed =
          ResolveRequiredProjection(binding, impl_->projection_resolver_);
      if (!observed.preflight_health.healthy_for_target(
              *state.active_projection)) {
        throw CanonicalEmptySceneOwnerConflictError(
            "native projection is not healthy at the active revision");
      }
      health.push_back(std::move(observed.preflight_health));
    } catch (const std::exception &error) {
      ErrorDTO unavailable{
          "active_native_projection_unavailable", error.what(), {}};
      health.push_back(core::ProjectionHealthDTO{
          binding,
          1U,
          ProjectionHealthState::kUnavailable,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          unavailable,
      });
      aggregate_error = std::move(unavailable);
    } catch (...) {
      ErrorDTO unavailable{
          "active_native_projection_unavailable",
          "native projection health threw a non-standard exception",
          {}};
      health.push_back(core::ProjectionHealthDTO{
          binding,
          1U,
          ProjectionHealthState::kUnavailable,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          unavailable,
      });
      aggregate_error = std::move(unavailable);
    }
  }
  if (!access.scene_writes_open || !access.projection_leases_open) {
    aggregate_error = ErrorDTO{
        "active_native_projection_recovery_pending",
        "active native projection recovery or scene transition is incomplete",
        {}};
  }
  ProjectionAggregateHealthDTO aggregate{
      std::move(source),
      aggregate_error.has_value() ? ProjectionHealthState::kUnavailable
                                  : ProjectionHealthState::kHealthy,
      std::move(health),
      std::move(aggregate_error),
  };
  if (!aggregate.valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "active native projection set cannot form aggregate health");
  }
  return aggregate;
}

core::Result<core::SceneCandidatePreviewDTO>
CanonicalEmptySceneOwner::PreviewResolvedSceneCandidate(
    const core::ResolvedSceneCandidatePreviewRequestDTO &preview) const {
  const auto reject = [](std::string code, std::string message,
                         std::map<std::string, std::string> details = {}) {
    return core::Result<core::SceneCandidatePreviewDTO>::Err(
        SceneError(code, message, std::move(details)));
  };
  const auto &request = preview.request;
  if (!std::holds_alternative<core::AttachCarriedObjectRequestDTO>(
          request.operation) &&
      !std::holds_alternative<core::DetachCarriedObjectRequestDTO>(
          request.operation)) {
    return reject("invalid_request",
                  "Candidate scene preview currently admits only carried-"
                  "object attach or detach",
                  {{"reason", "scene_candidate_operation_unsupported"}});
  }
  try {
    ValidateBoundedIdentity(request.request_context.request_id.value(),
                            "request_id");
    ValidateBoundedIdentity(request.request_context.request_owner_epoch.value(),
                            "request_owner_epoch");
    ValidateBoundedText(request.request_context.source_label, "source_label");
    if (request.request_context.correlation_id.has_value()) {
      ValidateBoundedText(*request.request_context.correlation_id,
                          "correlation_id");
    }
    if (!request.attribution.locally_valid() ||
        !request.expected_projection_revision.valid() ||
        !preview.frame_resolution.has_value() ||
        !preview.frame_resolution->valid() ||
        !preview.frame_resolution->predicted_state_digest.has_value() ||
        preview.frame_resolution->projection_binding.projection_revision !=
            request.expected_projection_revision) {
      throw std::invalid_argument(
          "candidate scene request or frame resolution is invalid");
    }
  } catch (const std::exception &error) {
    return reject("invalid_request", error.what(),
                  {{"reason", "scene_candidate_request_invalid"}});
  }

  std::string request_digest_input;
  ContentDigest request_digest(std::string{});
  try {
    request_digest_input =
        logical_scene_codec_internal::EncodeRequestDigestInput(request);
    request_digest = ContentDigest(
        manyforge::content_identity::sha256_digest(request_digest_input));
  } catch (const std::exception &error) {
    return reject("invalid_request", error.what(),
                  {{"reason", "scene_candidate_request_not_canonical"}});
  }

  DurableState observed_state;
  std::optional<SceneTransactionStoreState> observed_transactions;
  {
    std::lock_guard lock(impl_->mutex_);
    const auto &cached = impl_->ReadSceneCommitState();
    observed_state = cached.durable_state;
    observed_transactions = cached.transaction_state;
    if (!observed_state.active_projection.has_value() ||
        !observed_state.active_scene_snapshot.has_value() ||
        !observed_transactions.has_value()) {
      return reject("live_scene_snapshot_unavailable",
                    "Logical-scene bootstrap is not finalized",
                    {{"reason", "scene_candidate_snapshot_unavailable"}});
    }
    if (request.request_context.request_owner_epoch !=
        observed_transactions->request_owner_epoch) {
      return reject("stale_request_owner_epoch",
                    "Candidate scene request names a stale owner epoch",
                    {{"expected_request_owner_epoch",
                      observed_transactions->request_owner_epoch.value()}});
    }
    const auto access = AccessStatus(observed_state);
    if (!access.scene_writes_open) {
      return reject("live_scene_snapshot_unavailable",
                    "Logical-scene projection is unavailable or changing",
                    {{"reason", "scene_candidate_writes_fenced"}});
    }
    if (request.expected_projection_revision !=
        *observed_state.active_projection) {
      return reject("stale_revision",
                    "Candidate scene request does not name the active "
                    "projection",
                    {{"reason", "scene_candidate_projection_stale"}});
    }
  }

  std::vector<RequiredSceneProjection> projections;
  const auto required_bindings = ActiveNativeRuntimeBindings(observed_state);
  projections.reserve(required_bindings.size());
  for (const auto &binding : required_bindings) {
    try {
      auto projection =
          ResolveRequiredProjection(binding, impl_->projection_resolver_);
      if (!projection.preflight_health.healthy_for_target(
              *observed_state.active_projection)) {
        return reject(
            "live_scene_snapshot_unavailable",
            "A required native projection is not healthy at the candidate "
            "predecessor",
            {{"reason", "scene_candidate_native_projection_unhealthy"}});
      }
      projections.push_back(std::move(projection));
    } catch (const std::exception &error) {
      return reject(
          "live_scene_snapshot_unavailable", error.what(),
          {{"reason", "scene_candidate_native_projection_unavailable"}});
    } catch (...) {
      return reject(
          "live_scene_snapshot_unavailable",
          "Native scene projection raised a non-standard exception",
          {{"reason", "scene_candidate_native_projection_unavailable"}});
    }
  }

  try {
    auto candidate =
        ResolveSceneCandidate(request, *observed_state.active_scene_snapshot,
                              observed_transactions->next_transaction_sequence,
                              request_digest, preview.frame_resolution);
    auto effective_policy =
        ProjectEffectivePolicy(candidate.snapshot, projections);
    const core::ProjectionRevisionDTO proposed{
        observed_state.active_projection->retained_bundle_id,
        observed_state.active_projection->graph_revision,
        candidate.snapshot.revision};
    auto result = core::SceneCandidatePreviewDTO{
        request_digest,
        *observed_state.active_projection,
        proposed,
        std::make_shared<const LogicalSceneSnapshotDTO>(
            std::move(candidate.snapshot)),
        std::move(candidate.operation),
        std::move(effective_policy),
        candidate.update_status,
        candidate.resolution_evidence_digest};
    if (!result.valid()) {
      return reject("invariant_violation",
                    "Scene candidate reducer produced an invalid successor",
                    {{"reason", "scene_candidate_result_invalid"}});
    }
    return core::Result<core::SceneCandidatePreviewDTO>::Ok(std::move(result));
  } catch (const SceneRequestRejectionError &error) {
    return reject(error.code(), error.what(), error.details());
  } catch (const CanonicalEmptySceneOwnerConflictError &error) {
    return reject("scene_object_state_conflict", error.what());
  } catch (const CanonicalEmptySceneOwnerLimitError &error) {
    return reject("request_ledger_full", error.what());
  } catch (const std::invalid_argument &error) {
    return reject("invalid_request", error.what());
  } catch (const std::exception &error) {
    return reject("live_scene_snapshot_unavailable", error.what());
  } catch (...) {
    return reject("live_scene_snapshot_unavailable",
                  "Scene candidate preview raised a non-standard exception");
  }
}

SceneCommitResultDTO
CanonicalEmptySceneOwner::Commit(const SceneTransactionRequestDTO &request) {
  if (request.request_context.request_id.empty()) {
    throw std::invalid_argument("scene request_id must be non-empty");
  }
  const auto reject = [&](const core::RequestReplayStatusDTO replay_status,
                          const std::string_view code,
                          const std::string_view message,
                          std::map<std::string, std::string> details = {}) {
    return FailedSceneResult(request.request_context.request_id, replay_status,
                             SceneError(code, message, std::move(details)),
                             SceneProjectionCommitStatusDTO::kNotAttempted);
  };

  try {
    ValidateBoundedIdentity(request.request_context.request_id.value(),
                            "request_id");
    ValidateBoundedIdentity(request.request_context.request_owner_epoch.value(),
                            "request_owner_epoch");
    ValidateBoundedText(request.request_context.source_label, "source_label");
    if (request.request_context.correlation_id.has_value()) {
      ValidateBoundedText(*request.request_context.correlation_id,
                          "correlation_id");
    }
    if (!request.attribution.locally_valid()) {
      throw std::invalid_argument("request attribution is invalid");
    }
    if (request.attribution.principal_id.has_value()) {
      ValidateBoundedText(*request.attribution.principal_id, "principal_id");
    }
    if (!request.expected_projection_revision.valid()) {
      throw std::invalid_argument("expected projection revision is invalid");
    }
  } catch (const std::invalid_argument &error) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew, "invalid_request",
                  error.what());
  }

  const bool authored_mutation = IsAuthoredSceneMutation(request);
  if (authored_mutation &&
      !request.expected_admission_generation.has_value()) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                  "authoring_not_ready",
                  "authored scene mutation requires an admission generation");
  }
  if (!authored_mutation && request.expected_admission_generation.has_value()) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                  "invalid_request",
                  "runtime-owned scene mutation cannot carry authoring admission");
  }

  std::string request_json;
  std::string request_digest_input;
  ContentDigest request_digest(std::string{});
  try {
    request_json = logical_scene_codec_internal::EncodeRequest(request);
    if (request_json.empty() ||
        request_json.size() > kMaximumLogicalSceneTransactionBytes) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "invalid_request",
                    "scene request exceeds its canonical byte bound");
    }
    request_digest_input =
        logical_scene_codec_internal::EncodeRequestDigestInput(request);
    request_digest = ContentDigest(
        manyforge::content_identity::sha256_digest(request_digest_input));
  } catch (const std::exception &error) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew, "invalid_request",
                  error.what());
  }

  std::lock_guard lock(impl_->mutex_);
  auto *database = impl_->database();
  DurableState observed_state;
  std::optional<SceneTransactionStoreState> observed_transactions;
  std::optional<ResolvedSceneCandidate> candidate;
  std::int64_t external_data_version = 0;

  {
    const auto &cached = impl_->ReadSceneCommitState();
    observed_state = cached.durable_state;
    observed_transactions = cached.transaction_state;
    external_data_version = cached.external_data_version;
    if (!observed_state.active_projection.has_value() ||
        !observed_state.active_scene_snapshot.has_value()) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable",
                    "logical-scene bootstrap is not finalized");
    }
    if (!observed_transactions.has_value()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "finalized logical scene has no reconstructed transaction state");
    }
    const auto &owner_epoch =
        observed_transactions->request_owner_epoch.value();
    if (request.request_context.request_owner_epoch.value() != owner_epoch) {
      return reject(core::RequestReplayStatusDTO::kStaleRequestOwnerEpoch,
                    "stale_request_owner_epoch",
                    "scene request names a stale logical-owner epoch",
                    {{"expected_request_owner_epoch", owner_epoch}});
    }
    if (const auto existing = ReadOutcomeForRequest(
            database, owner_epoch, request.request_context.request_id);
        existing.has_value()) {
      if (existing->request_digest != request_digest) {
        return reject(core::RequestReplayStatusDTO::kRequestIdConflict,
                      "request_id_conflict",
                      "scene request ID was reused with different intent");
      }
      if (impl_->cell_admission_ != nullptr) {
        try {
          SettleAuthoredSceneClaimIfHeld(
              *impl_->cell_admission_, request, request_digest,
              existing->success, existing->projection_status);
        } catch (const CellAdmissionTemporarilyUnavailable &) {
          // Exact replay remains deterministic while the retained Kernel
          // claim, if any, continues to keep authoring fail-closed.
        }
      }
      return ResultFromStoredOutcome(
          database, *existing, core::RequestReplayStatusDTO::kReplayedExact);
    }
    if (authored_mutation && impl_->cell_admission_ == nullptr) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "authoring_not_ready",
                    "new authored scene mutation requires owner-local "
                    "admission");
    }
    const auto access = AccessStatus(observed_state);
    if (!access.scene_writes_open) {
      if (observed_state.OpenNamedApply() != nullptr) {
        return reject(
            core::RequestReplayStatusDTO::kAcceptedNew,
            "scene_projection_transition_in_progress",
            "a structural projection transition currently fences scene "
            "writes");
      }
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable",
                    "logical-scene projection is unavailable or "
                    "desynchronized");
    }
    if (request.expected_projection_revision !=
        *observed_state.active_projection) {
      return reject(
          core::RequestReplayStatusDTO::kAcceptedNew, "stale_revision",
          "expected composite projection revision is no longer active");
    }
    if (observed_transactions->resident_transaction_count >=
        kMaximumLogicalSceneTransactions) {
      Transaction compaction(database, "BEGIN IMMEDIATE");
      if (!impl_->SceneCommitStateStillCurrent(external_data_version)) {
        impl_->InvalidateSceneCommitState();
        return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                      "stale_revision",
                      "logical-scene state changed before history compaction");
      }
      impl_->InvalidateSceneCommitState();
      const auto next_epoch = CompactSceneTransactionHistory(
          database, observed_state, *observed_transactions);
      compaction.Commit();
      observed_transactions->request_owner_epoch = next_epoch;
      observed_transactions->resident_transaction_count = 0U;
      impl_->StoreSceneCommitState(observed_state,
                                   std::move(observed_transactions),
                                   external_data_version);
      return reject(
          core::RequestReplayStatusDTO::kStaleRequestOwnerEpoch,
          "stale_request_owner_epoch",
          "logical-scene request owner epoch rotated after bounded history "
          "compaction",
          {{"expected_request_owner_epoch", next_epoch.value()}});
    }
  }

  std::vector<RequiredSceneProjection> projections;
  const auto required_bindings = ActiveNativeRuntimeBindings(observed_state);
  projections.reserve(required_bindings.size());
  for (const auto &binding : required_bindings) {
    try {
      auto projection =
          ResolveRequiredProjection(binding, impl_->projection_resolver_);
      if (!projection.preflight_health.healthy_for_target(
              *observed_state.active_projection)) {
        return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                      "live_scene_snapshot_unavailable",
                      "a required native scene projection is not healthy at "
                      "the exact predecessor revision");
      }
      projections.push_back(std::move(projection));
    } catch (const std::exception &error) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable", error.what());
    } catch (...) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable",
                    "native scene projection health threw a non-standard "
                    "exception");
    }
  }

  try {
    const auto frame_resolution = ResolveFrameForRequest(
        request, *observed_state.active_scene_snapshot, projections);
    candidate =
        ResolveSceneCandidate(request, *observed_state.active_scene_snapshot,
                              observed_transactions->next_transaction_sequence,
                              request_digest, frame_resolution);
    auto effective_policy =
        ProjectEffectivePolicy(candidate->snapshot, projections);
    if (auto *policy = std::get_if<core::ResolvedCollisionPolicyDTO>(
            &candidate->operation)) {
      policy->effective_policy = std::move(effective_policy);
    }
  } catch (const SceneRequestRejectionError &error) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew, error.code(),
                  error.what(), error.details());
  } catch (const CanonicalEmptySceneOwnerConflictError &error) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                  "scene_object_state_conflict", error.what());
  } catch (const CanonicalEmptySceneOwnerLimitError &error) {
    return reject(core::RequestReplayStatusDTO::kRequestLedgerFull,
                  "request_ledger_full", error.what());
  } catch (const std::invalid_argument &error) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew, "invalid_request",
                  error.what());
  } catch (const std::exception &error) {
    return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                  "live_scene_snapshot_unavailable", error.what());
  }

  core::SceneTransactionDTO scene_transaction{
      request.request_context.request_id,
      request.attribution,
      observed_transactions->next_transaction_sequence,
      *observed_state.active_projection,
      ProjectionRevisionDTO{
          observed_state.active_projection->retained_bundle_id,
          observed_state.active_projection->graph_revision,
          candidate->snapshot.revision,
      },
      request_digest,
      candidate->operation,
      candidate->resolution_evidence_digest,
  };

  const auto reconstructed = core::ApplyResolvedSceneTransaction(
      *observed_state.active_scene_snapshot, scene_transaction);
  if (!reconstructed.ok()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "owner-normalized scene transaction was rejected by the shared "
        "projection reducer: " +
        reconstructed.error().message);
  }
  if (reconstructed.value() != candidate->snapshot) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "owner-normalized scene transaction does not reproduce its exact "
        "candidate snapshot");
  }

  {
    Transaction write(database, "BEGIN IMMEDIATE");
    if (!impl_->SceneCommitStateStillCurrent(external_data_version) ||
        NextSceneOwnerEventSequence(database) !=
            observed_transactions->next_owner_event_sequence ||
        ReadOutcomeForRequest(
            database, request.request_context.request_owner_epoch.value(),
            request.request_context.request_id)
            .has_value()) {
      impl_->InvalidateSceneCommitState();
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "stale_revision",
                    "logical-scene state changed before durable acceptance");
    }
    impl_->InvalidateSceneCommitState();
    InsertSceneIntent(database, request, request_digest, request_json,
                      scene_transaction, *candidate, projections.size());
    InsertSceneTransactionOwnerEvent(
        database, observed_transactions->next_owner_event_sequence,
        scene_transaction.transaction_sequence);
    write.Commit();
  }

  std::optional<core::CellAdmissionClaimDTO> authored_claim;
  if (authored_mutation) {
    try {
      authored_claim = RequireAuthoredSceneClaim(
          *impl_->cell_admission_,
          AuthoredSceneClaimIntent(request, request_digest));
    } catch (const std::exception &error) {
      Transaction outcome(database, "BEGIN IMMEDIATE");
      InsertSceneOutcome(database,
                         request.request_context.request_owner_epoch.value(),
                         request.request_context.request_id, request_digest,
                         scene_transaction.transaction_sequence, false,
                         SceneProjectionCommitStatusDTO::kNotAttempted, 0U,
                         SceneError("authoring_not_ready", error.what()));
      outcome.Commit();
      return FailedSceneResult(
          request.request_context.request_id,
          core::RequestReplayStatusDTO::kAcceptedNew,
          SceneError("authoring_not_ready", error.what()),
          SceneProjectionCommitStatusDTO::kNotAttempted);
    }
  }
  if (!projections.empty()) {
    Transaction attempt(database, "BEGIN IMMEDIATE");
    InsertProjectionAttempt(
        database, scene_transaction.transaction_sequence, 0U,
        projections.front().preflight_health.native_runtime_binding);
    attempt.Commit();
  }

  bool every_attempt_rolled_back = true;
  for (std::size_t ordinal = 0U; ordinal < projections.size(); ++ordinal) {
    const auto &projection = projections[ordinal];
    core::ProjectionCommitResultDTO projection_result = [&] {
      try {
        return NormalizeProjectionResult(
            scene_transaction, projection.preflight_health,
            projection.projection->ApplyTransaction(scene_transaction));
      } catch (const std::exception &error) {
        return UncertainProjectionResult(
            scene_transaction, projection.preflight_health,
            SceneError("scene_projection_call_failed", error.what()));
      } catch (...) {
        return UncertainProjectionResult(
            scene_transaction, projection.preflight_health,
            SceneError("scene_projection_call_failed",
                       "native scene projection threw a non-standard "
                       "exception"));
      }
    }();
    every_attempt_rolled_back =
        every_attempt_rolled_back &&
        projection_result.result ==
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven;
    const bool committed = projection_result.result ==
                           core::ProjectionCommitResultStateDTO::kCommitted;

    Transaction result_write(database, "BEGIN IMMEDIATE");
    InsertProjectionResult(database, scene_transaction.transaction_sequence,
                           ordinal, projection_result);
    if (!committed) {
      const auto status =
          every_attempt_rolled_back
              ? SceneProjectionCommitStatusDTO::kRejectedRollbackProven
              : SceneProjectionCommitStatusDTO::kFailedProjectionUncertain;
      ErrorDTO error =
          status == SceneProjectionCommitStatusDTO::kRejectedRollbackProven
              ? *projection_result.error
              : SceneError(
                    "scene_projection_commit_uncertain",
                    "at least one invoked native projection cannot prove the "
                    "predecessor state");
      InsertSceneOutcome(database,
                         request.request_context.request_owner_epoch.value(),
                         request.request_context.request_id, request_digest,
                         scene_transaction.transaction_sequence, false, status,
                         ordinal + 1U, error);
      if (status ==
          SceneProjectionCommitStatusDTO::kFailedProjectionUncertain) {
        const auto failed_state =
            ReadCompleteDurableState(database, impl_->fixed_query_);
        if (AccessStatus(failed_state).projection_leases_open) {
          throw CanonicalEmptySceneOwnerIntegrityError(
              "uncertain projection result did not close projection leases");
        }
      }
      result_write.Commit();
      if (status == SceneProjectionCommitStatusDTO::kRejectedRollbackProven) {
        if (authored_claim.has_value()) {
          ReleaseAuthoredSceneClaim(
              *impl_->cell_admission_, *authored_claim, request_digest,
              core::CellAdmissionSettlementKindDTO::kProvenNoEffect);
        }
        ++observed_transactions->next_transaction_sequence;
        ++observed_transactions->next_owner_event_sequence;
        ++observed_transactions->resident_transaction_count;
        if (impl_->SceneCommitStateStillCurrent(external_data_version)) {
          impl_->StoreSceneCommitState(observed_state, observed_transactions,
                                       external_data_version);
        }
      }
      return FailedSceneResult(request.request_context.request_id,
                               core::RequestReplayStatusDTO::kAcceptedNew,
                               std::move(error), status);
    }
    if (ordinal + 1U < projections.size()) {
      InsertProjectionAttempt(
          database, scene_transaction.transaction_sequence, ordinal + 1U,
          projections[ordinal + 1U].preflight_health.native_runtime_binding);
    } else {
      InsertSceneOutcome(database,
                         request.request_context.request_owner_epoch.value(),
                         request.request_context.request_id, request_digest,
                         scene_transaction.transaction_sequence, true,
                         SceneProjectionCommitStatusDTO::kCommitted,
                         projections.size(), std::nullopt);
    }
    result_write.Commit();
  }

  if (projections.empty()) {
    Transaction outcome(database, "BEGIN IMMEDIATE");
    InsertSceneOutcome(database,
                       request.request_context.request_owner_epoch.value(),
                       request.request_context.request_id, request_digest,
                       scene_transaction.transaction_sequence, true,
                       SceneProjectionCommitStatusDTO::kCommitted,
                       projections.size(), std::nullopt);
    outcome.Commit();
  }

  if (authored_claim.has_value()) {
    ReleaseAuthoredSceneClaim(
        *impl_->cell_admission_, *authored_claim, request_digest,
        core::CellAdmissionSettlementKindDTO::kOwnerCommit);
  }

  observed_state.active_projection =
      scene_transaction.proposed_projection_revision;
  observed_state.active_scene_snapshot = candidate->snapshot;
  observed_state.projection_desynchronized = false;
  observed_state.unresolved_scene_transaction = false;
  observed_state.latest_uncertain_transaction_sequence.reset();
  observed_transactions->active_projection =
      scene_transaction.proposed_projection_revision;
  observed_transactions->active_snapshot = candidate->snapshot;
  ++observed_transactions->next_transaction_sequence;
  ++observed_transactions->next_owner_event_sequence;
  ++observed_transactions->resident_transaction_count;
  observed_transactions->projection_desynchronized = false;
  observed_transactions->unresolved_native_attempt = false;
  observed_transactions->unresolved_transaction_sequence.reset();
  observed_transactions->latest_uncertain_transaction_sequence.reset();
  if (!impl_->SceneCommitStateStillCurrent(external_data_version)) {
    throw CanonicalEmptySceneOwnerConflictError(
        "logical-scene database changed while completing a scene commit");
  }
  impl_->StoreSceneCommitState(observed_state, observed_transactions,
                               external_data_version);

  SceneCommitResultDTO result{
      request.request_context.request_id,
      core::RequestReplayStatusDTO::kAcceptedNew,
      true,
      std::nullopt,
      scene_transaction.transaction_sequence,
      scene_transaction.proposed_projection_revision,
      SceneProjectionCommitStatusDTO::kCommitted,
      candidate->update_status,
  };
  if (!result.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "successful scene commit cannot form a valid public result");
  }
  return result;
}

core::Result<void>
CanonicalEmptySceneOwner::ReconcileAuthoredSceneAdmissionClaims() {
  std::lock_guard lock(impl_->mutex_);
  if (impl_->cell_admission_ == nullptr) {
    return core::Result<void>::Err(
        {"kernel_owner_unavailable",
         "authored scene admission owner is not configured", {}});
  }
  try {
    SettleRecoveredAuthoredSceneClaims(impl_->database(),
                                       *impl_->cell_admission_);
    return core::Result<void>::Ok();
  } catch (const CellAdmissionTemporarilyUnavailable &error) {
    return core::Result<void>::Err(
        {"kernel_owner_unavailable", error.what(), {}});
  } catch (const std::exception &error) {
    return core::Result<void>::Err(
        {"authored_scene_admission_reconciliation_failed", error.what(), {}});
  } catch (...) {
    return core::Result<void>::Err(
        {"authored_scene_admission_reconciliation_failed",
         "authored scene admission reconciliation raised a non-standard "
         "exception",
         {}});
  }
}

RequestOwnerEpoch CanonicalEmptySceneOwner::CurrentRequestOwnerEpoch() const {
  std::lock_guard lock(impl_->mutex_);
  const auto &cached = impl_->ReadSceneCommitState();
  const auto &state = cached.durable_state;
  if (!state.active_projection.has_value() ||
      !state.active_scene_snapshot.has_value() ||
      !cached.transaction_state.has_value()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "logical-scene request owner epoch requires finalized bootstrap");
  }
  return cached.transaction_state->request_owner_epoch;
}

ProjectionRevisionDTO
CanonicalEmptySceneOwner::CurrentProjectionRevision() const {
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadSceneCommitState().durable_state;
  if (!AccessStatus(state).projection_leases_open ||
      !state.active_projection.has_value()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "live logical-scene projection is unavailable");
  }
  return *state.active_projection;
}

std::shared_ptr<const LogicalSceneSnapshotDTO>
CanonicalEmptySceneOwner::LogicalSnapshot() const {
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadSceneCommitState().durable_state;
  if (!AccessStatus(state).projection_leases_open ||
      !state.active_scene_snapshot.has_value()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "live logical-scene snapshot is unavailable");
  }
  return std::make_shared<const LogicalSceneSnapshotDTO>(
      *state.active_scene_snapshot);
}

std::vector<SceneObjectSnapshotDTO>
CanonicalEmptySceneOwner::GetSceneObjects() const {
  return LogicalSnapshot()->objects;
}

SceneObjectStateDTO CanonicalEmptySceneOwner::GetObjectState(
    const core::SceneResourceId &scene_resource_id) const {
  ValidateBoundedIdentity(scene_resource_id.value(), "scene_resource_id");
  const auto snapshot = LogicalSnapshot();
  const auto found = std::lower_bound(
      snapshot->objects.begin(), snapshot->objects.end(), scene_resource_id,
      [](const SceneObjectSnapshotDTO &object,
         const core::SceneResourceId &id) {
        return object.scene_resource_id < id;
      });
  if (found == snapshot->objects.end() ||
      found->scene_resource_id != scene_resource_id) {
    return {scene_resource_id, false, false, std::nullopt, std::nullopt};
  }
  const auto *carried =
      std::get_if<core::CarriedSceneObjectPlacementDTO>(&found->placement);
  return {
      scene_resource_id,
      true,
      carried != nullptr,
      carried == nullptr ? std::nullopt
                         : std::optional<core::CarriedObjectAttachmentId>(
                               carried->carried_object_attachment_id),
      found->provider_evidence,
  };
}

core::Result<core::Pose3DTO> CanonicalEmptySceneOwner::GetObjectPose(
    const core::SceneResourceId &scene_resource_id) const {
  ValidateBoundedIdentity(scene_resource_id.value(), "scene_resource_id");
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadSceneCommitState().durable_state;
  if (!AccessStatus(state).projection_leases_open ||
      !state.active_projection.has_value() ||
      !state.active_scene_snapshot.has_value()) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError("live_scene_snapshot_unavailable",
                   "logical-scene projection is unavailable"));
  }
  const auto snapshot = std::make_shared<const LogicalSceneSnapshotDTO>(
      *state.active_scene_snapshot);
  const auto found = std::lower_bound(
      snapshot->objects.begin(), snapshot->objects.end(), scene_resource_id,
      [](const SceneObjectSnapshotDTO &object,
         const core::SceneResourceId &id) {
        return object.scene_resource_id < id;
      });
  if (found == snapshot->objects.end() ||
      found->scene_resource_id != scene_resource_id) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError("object_not_found", "scene resource does not exist",
                   {{"scene_resource_id", scene_resource_id.value()}}));
  }
  const auto *fixed =
      std::get_if<core::CellFixedSceneObjectPlacementDTO>(&found->placement);
  if (fixed != nullptr) {
    return core::Result<core::Pose3DTO>::Ok(fixed->pose_in_cell);
  }
  const auto *carried =
      std::get_if<core::CarriedSceneObjectPlacementDTO>(&found->placement);
  if (carried == nullptr) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError("scene_object_state_conflict",
                   "scene resource has an unknown placement state",
                   {{"scene_resource_id", scene_resource_id.value()}}));
  }
  const auto attachment =
      std::lower_bound(snapshot->carried_object_attachments.begin(),
                       snapshot->carried_object_attachments.end(),
                       carried->carried_object_attachment_id,
                       [](const core::CarriedObjectAttachmentDTO &candidate,
                          const core::CarriedObjectAttachmentId &id) {
                         return candidate.carried_object_attachment_id < id;
                       });
  if (attachment == snapshot->carried_object_attachments.end() ||
      attachment->carried_object_attachment_id !=
          carried->carried_object_attachment_id ||
      attachment->carried_object_attachment_revision !=
          carried->carried_object_attachment_revision) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError("scene_object_state_conflict",
                   "carried scene resource has no exact attachment row",
                   {{"scene_resource_id", scene_resource_id.value()}}));
  }

  std::vector<RequiredSceneProjection> projections;
  try {
    const auto bindings = ActiveNativeRuntimeBindings(state);
    projections.reserve(bindings.size());
    for (const auto &binding : bindings) {
      auto projection =
          ResolveRequiredProjection(binding, impl_->projection_resolver_);
      if (!projection.preflight_health.healthy_for_target(
              *state.active_projection)) {
        throw CanonicalEmptySceneOwnerConflictError(
            "native projection is not healthy at the carried-object "
            "revision");
      }
      projections.push_back(std::move(projection));
    }
    const auto resolution = ResolveFrameAcrossProjections(
        core::SceneFrameResolutionSelectorDTO{attachment->carrier_frame_ref},
        *state.active_projection, projections,
        "object-pose-" + scene_resource_id.value());
    if (!resolution.has_value()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "carried-object frame resolution returned no receipt");
    }
    return core::Result<core::Pose3DTO>::Ok(core::multiply_poses(
        resolution->frame_pose_in_cell, attachment->pose_in_carrier_frame));
  } catch (const SceneRequestRejectionError &error) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError(error.code(), error.what(), error.details()));
  } catch (const std::exception &error) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError("live_scene_snapshot_unavailable", error.what(),
                   {{"scene_resource_id", scene_resource_id.value()}}));
  }
}

CanonicalEmptySceneAccessStatus
CanonicalEmptySceneOwner::ReadAccessStatus() const {
  std::lock_guard lock(impl_->mutex_);
  auto status = AccessStatus(impl_->ReadSceneCommitState().durable_state);
  if (!status.valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "durable state cannot form a valid access-gate status");
  }
  return status;
}

std::optional<LogicalSceneSnapshotRoot>
CanonicalEmptySceneOwner::ReadActiveSnapshot() const {
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadSceneCommitState().durable_state;
  if (!state.active_projection.has_value() ||
      !state.active_scene_snapshot.has_value()) {
    return std::nullopt;
  }
  if (state.active_scene_snapshot->revision !=
          state.active_projection->scene_revision ||
      state.active_scene_snapshot->cell_id != impl_->fixed_query_.cell_id) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "active projection does not identify the durable logical-scene "
        "snapshot");
  }
  return LogicalSceneSnapshotRoot(
      std::make_shared<const LogicalSceneSnapshotDTO>(
          *state.active_scene_snapshot));
}

CanonicalEmptySceneStoreIntegrityReport
CanonicalEmptySceneOwner::VerifyIntegrity() const {
  std::lock_guard lock(impl_->mutex_);
  auto *database = impl_->database();
  Transaction transaction(database, "BEGIN");
  ValidateDatabase(database, impl_->fixed_query_, impl_->admission_digest_);
  const auto state = ReadCompleteDurableState(database, impl_->fixed_query_);
  CanonicalEmptySceneStoreIntegrityReport report{
      impl_->fixed_query_,
      std::string(kLogicalSceneStoreSchemaId),
      kLogicalSceneStoreSchemaVersion,
      CurrentSchemaArtifactDigest(),
      ScalarInteger(database,
                    "SELECT count(*) FROM canonical_empty_scene_acceptance"),
      ScalarInteger(database,
                    "SELECT count(*) FROM canonical_empty_scene_rebuild"),
      ScalarInteger(database,
                    "SELECT count(*) FROM canonical_empty_scene_finalization"),
      ScalarInteger(database,
                    "SELECT count(*) FROM logical_scene_named_apply_stages"),
      ScalarInteger(
          database,
          "SELECT count(*) FROM logical_scene_named_apply_activations"),
      ScalarInteger(database,
                    "SELECT count(*) FROM logical_scene_named_apply_discards"),
      ScalarInteger(
          database,
          "SELECT count(*) FROM logical_scene_named_apply_finalizations"),
      ScalarInteger(
          database,
          "SELECT count(*) FROM logical_scene_native_runtime_recovery_intents"),
      ScalarInteger(database,
                    "SELECT count(*) FROM "
                    "logical_scene_native_runtime_recovery_completions"),
      ScalarInteger(database, "SELECT count(*) FROM "
                              "logical_scene_native_runtime_recovery_bindings"),
      ScalarInteger(
          database,
          "SELECT count(*) FROM logical_scene_native_runtime_recovery_results"),
      ScalarInteger(database,
                    "SELECT count(*) FROM logical_scene_transaction_intents"),
      ScalarInteger(database,
                    "SELECT count(*) FROM logical_scene_request_outcomes"),
      ScalarInteger(database,
                    "SELECT count(*) FROM logical_scene_projection_attempts"),
      ScalarInteger(database,
                    "SELECT count(*) FROM logical_scene_projection_results"),
      ScalarInteger(database,
                    "SELECT count(*) FROM logical_scene_owner_events"),
      state.projection_desynchronized || state.unresolved_scene_transaction ||
          (state.LatestActiveRuntimeRecovery() != nullptr &&
           !state.LatestActiveRuntimeRecovery()->completion_health.has_value()),
      AccessStatus(state),
  };
  transaction.Commit();
  if (!report.access.valid() || report.acceptance_row_count < 0 ||
      report.acceptance_row_count > 1 || report.rebuild_row_count < 0 ||
      report.rebuild_row_count > 1 || report.finalization_row_count < 0 ||
      report.finalization_row_count > 1 || report.named_apply_stage_count < 0 ||
      report.named_apply_stage_count >
          static_cast<std::int64_t>(kMaximumNamedApplySceneStages) ||
      report.named_apply_activation_count < 0 ||
      report.named_apply_activation_count > report.named_apply_stage_count ||
      report.named_apply_discard_count < 0 ||
      report.named_apply_discard_count > report.named_apply_stage_count ||
      report.named_apply_finalization_count < 0 ||
      report.named_apply_finalization_count >
          report.named_apply_activation_count ||
      report.native_recovery_intent_count < 0 ||
      report.native_recovery_intent_count >
          static_cast<std::int64_t>(
              kMaximumRetainedNativeProjectionRecoveries) ||
      report.native_recovery_completion_count < 0 ||
      report.native_recovery_completion_count >
          report.native_recovery_intent_count ||
      report.native_recovery_binding_count < 0 ||
      report.native_recovery_binding_count >
          report.native_recovery_intent_count *
              static_cast<std::int64_t>(
                  kMaximumNativeProjectionTransitionRows) ||
      report.native_recovery_result_count < 0 ||
      report.native_recovery_result_count >
          report.native_recovery_binding_count ||
      report.transaction_intent_count < 0 ||
      report.transaction_intent_count >
          static_cast<std::int64_t>(kMaximumLogicalSceneTransactions) ||
      report.request_outcome_count < 0 ||
      report.request_outcome_count > report.transaction_intent_count ||
      report.projection_attempt_count < 0 ||
      report.projection_attempt_count >
          static_cast<std::int64_t>(kMaximumLogicalSceneTransactions * 256U) ||
      report.projection_result_count < 0 ||
      report.projection_result_count > report.projection_attempt_count ||
      report.owner_event_count < 0 ||
      report.owner_event_count >
          static_cast<std::int64_t>(kMaximumLogicalSceneOwnerEvents)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene integrity report violates fixed row bounds");
  }
  return report;
}

void CanonicalEmptySceneOwner::Close() { impl_->Close(); }

core::Result<void> ValidateAuthoredSceneCellAdmissionSettlementReadOnly(
    const std::filesystem::path &database_path, const core::CellId &cell_id,
    const core::CellAdmissionClaimDTO &claim,
    const core::CellAdmissionClaimReleaseDTO &release) {
  const auto rejected = [](std::string reason) {
    return core::Result<void>::Err(core::ErrorDTO{
        "invariant_violation",
        "authored-scene cell-admission settlement is not owner-proven",
        {{"reason", std::move(reason)}}});
  };
  if (!core::ValidateCellAdmissionClaimIntent(claim.intent).ok() ||
      claim.intent.claim_kind !=
          core::CellAdmissionClaimKindDTO::kAuthoredScene ||
      claim.intent.owner_role !=
          core::CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner ||
      release.owner_role != claim.intent.owner_role ||
      release.owner_epoch != claim.intent.owner_epoch ||
      release.owner_transaction_id != claim.intent.owner_transaction_id ||
      release.request_id != claim.intent.request_id ||
      release.request_payload_digest != claim.intent.request_payload_digest ||
      release.acquired_admission_generation !=
          claim.acquired_admission_generation ||
      release.claim_generation != claim.claim_generation) {
    return rejected("authored_scene_release_identity_mismatch");
  }

  sqlite3 *database = nullptr;
  const auto opened = sqlite3_open_v2(
      database_path.string().c_str(), &database,
      SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
      nullptr);
  if (opened != SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return rejected("logical_scene_journal_unavailable");
  }
  try {
    Check(database, sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds),
          "configure read-only scene-settlement timeout");
    Exec(database, "PRAGMA query_only=ON");
    Transaction snapshot(database, "BEGIN");
    if (ScalarInteger(database, "PRAGMA application_id") !=
            kCanonicalEmptySceneSqliteApplicationId ||
        ScalarInteger(database, "PRAGMA user_version") !=
            kLogicalSceneStoreSchemaVersion) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene settlement reader found a foreign schema");
    }
    Statement metadata(
        database,
        "SELECT cell_id FROM logical_scene_store_metadata WHERE singleton=1");
    if (!metadata.Step() || metadata.Text(0) != cell_id.value() ||
        metadata.Step()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene settlement reader found a foreign cell");
    }
    Statement row(
        database,
        "SELECT i.request_json,i.request_digest,o.success,o.projection_status "
        "FROM logical_scene_transaction_intents i "
        "JOIN logical_scene_request_outcomes o "
        "ON o.request_owner_epoch=i.request_owner_epoch "
        "AND o.request_id=i.request_id "
        "WHERE i.request_owner_epoch=? AND i.request_id=?");
    row.Bind(1, claim.intent.owner_epoch.value());
    row.Bind(2, claim.intent.request_id.value());
    if (!row.Step()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "authored-scene terminal outcome is absent");
    }
    const auto request =
        logical_scene_codec_internal::DecodeRequest(row.Text(0));
    const auto request_digest = row.Text(1);
    const bool success = row.Integer(2) == 1;
    const auto projection_status = DecodeProjectionStatus(row.Text(3));
    if (row.Step()) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "authored-scene terminal outcome is not unique");
    }
    snapshot.Commit();
    sqlite3_close(database);
    database = nullptr;
    const auto expected_kind =
        success ? core::CellAdmissionSettlementKindDTO::kOwnerCommit
                : core::CellAdmissionSettlementKindDTO::kProvenNoEffect;
    if (!IsAuthoredSceneMutation(request) ||
        request.expected_admission_generation !=
            std::optional<core::AdmissionGeneration>{
                claim.intent.expected_admission_generation} ||
        request.request_context.request_owner_epoch != claim.intent.owner_epoch ||
        request.request_context.request_id != claim.intent.request_id ||
        request_digest != claim.intent.request_payload_digest.value() ||
        release.settlement.evidence_digest.value() != request_digest ||
        release.settlement.settlement_kind != expected_kind ||
        (!success && projection_status !=
                         SceneProjectionCommitStatusDTO::kRejectedRollbackProven &&
         projection_status != SceneProjectionCommitStatusDTO::kNotAttempted)) {
      return rejected("authored_scene_terminal_evidence_mismatch");
    }
    return core::Result<void>::Ok();
  } catch (const std::exception &) {
    sqlite3_close(database);
    return rejected("logical_scene_journal_read_failed");
  }
}

} // namespace manyforge::scene
