#include "manyforge_scene/canonical_empty_scene_owner.hpp"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
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
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace manyforge::scene {
namespace {

using core::ActiveBundlePublicationGeneration;
using core::ActiveBundlePublicationReceiptDTO;
using core::ActiveBundlePublicationRequestDTO;
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
constexpr std::int64_t kExpectedSchemaObjectCardinality = 42;
constexpr std::uint64_t kMaximumSafeIntegral = 9007199254740991ULL;
constexpr int kMaximumSqliteValueBytes = 1024 * 1024;
constexpr std::size_t kMaximumLogicalSceneOwnerEvents =
    kMaximumLogicalSceneTransactions + kMaximumNamedApplySceneStages;

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
                "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name LIMIT 43");
  SchemaObjects objects;
  std::int64_t count = 0;
  while (statement.Step()) {
    ++count;
    if (count > kExpectedSchemaObjectCardinality) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene schema object cardinality exceeds v4");
    }
    objects.emplace(std::make_pair(statement.Text(0), statement.Text(1)),
                    NormalizedSql(statement.Text(2)));
  }
  return objects;
}

SchemaObjects BuildExpectedSchemaObjects(const bool include_transactions) {
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
    if (include_transactions) {
      Exec(database, detail::kEmbeddedLogicalSceneTransactionMigration);
    }
    auto objects = ReadSchemaObjects(database);
    const auto expected_count = include_transactions
                                    ? kExpectedSchemaObjectCardinality
                                    : kExpectedBaseSchemaObjectCardinality;
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
  static const SchemaObjects expected = BuildExpectedSchemaObjects(false);
  return expected;
}

const SchemaObjects &ExpectedSchemaObjects() {
  static const SchemaObjects expected = BuildExpectedSchemaObjects(true);
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

std::string CurrentSchemaArtifactDigest() {
  return manyforge::content_identity::sha256_digest(
      std::string(detail::kEmbeddedCanonicalEmptySceneSchema) + "\n--v4--\n" +
      std::string(detail::kEmbeddedLogicalSceneTransactionMigration));
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
};

struct DurableState final {
  std::optional<AcceptedState> accepted;
  bool rebuilt{};
  std::optional<ActiveBundlePublicationReceiptDTO> finalization_receipt;
  std::vector<NamedApplyStageState> named_apply_stages;
  std::optional<ProjectionRevisionDTO> active_projection;
  std::optional<LogicalSceneSnapshotDTO> active_scene_snapshot;
  bool projection_desynchronized{};
  bool unresolved_scene_transaction{};

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

struct SceneTransactionStoreState final {
  ProjectionRevisionDTO active_projection;
  LogicalSceneSnapshotDTO active_snapshot;
  std::uint64_t next_transaction_sequence;
  std::uint64_t next_owner_event_sequence;
  bool projection_desynchronized;
  bool unresolved_native_attempt;
  std::optional<std::uint64_t> unresolved_transaction_sequence;
};

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
  SceneTransactionStoreState state{
      bootstrap_projection, bootstrap_snapshot, 1U, 1U, false, false,
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
      "FROM logical_scene_transaction_intents ORDER BY "
      "transaction_sequence LIMIT 257");
  std::uint64_t expected_sequence = 1U;
  while (statement.Step()) {
    if (!records.empty() &&
        (!records.back().outcome.has_value() ||
         records.back().outcome->projection_status ==
             SceneProjectionCommitStatusDTO::kFailedProjectionUncertain)) {
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
    if (intent.request_owner_epoch !=
            bootstrap_snapshot.revision.scene_id.value() ||
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
        state.projection_desynchronized = true;
      }
    }
    records.push_back(
        StoredSceneRecord{std::move(intent), attempts, results, outcome});
    ++expected_sequence;
    state.next_transaction_sequence = expected_sequence;
  }
  if (expected_sequence > kMaximumLogicalSceneTransactions + 1U) {
    throw CanonicalEmptySceneOwnerLimitError(
        "logical-scene transaction ledger exceeds its fixed bound");
  }

  std::vector<bool> transaction_event_seen(records.size(), false);
  std::vector<bool> named_apply_event_seen(
      durable_state.named_apply_stages.size(), false);
  std::vector<ProjectionRevisionDTO> published_projections{
      bootstrap_projection};
  Statement events(
      database,
      "SELECT owner_event_sequence,event_kind,transaction_sequence,"
      "named_apply_lineage_generation FROM logical_scene_owner_events "
      "ORDER BY owner_event_sequence LIMIT 513");
  std::uint64_t expected_event_sequence = 1U;
  bool history_sealed = false;
  while (events.Step()) {
    if (history_sealed) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene owner history continues after an unresolved or "
          "desynchronized event");
    }
    if (expected_event_sequence > kMaximumLogicalSceneOwnerEvents ||
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
          static_cast<std::size_t>(sequence) > records.size()) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "scene transaction owner event references an absent intent");
      }
      const auto index = static_cast<std::size_t>(sequence - 1);
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
                 SceneProjectionCommitStatusDTO::kFailedProjectionUncertain) {
        history_sealed = true;
      }
    } else if (kind == "named_apply_finalized") {
      if (!events.IsNull(2) || events.IsNull(3)) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "named Apply owner event has invalid referent presence");
      }
      const auto lineage = events.Integer(3);
      if (lineage <= 0 || static_cast<std::size_t>(lineage) >
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

const core::CellSceneFrameRefDTO &
RequireCellFrame(const core::SceneFrameRefDTO &frame, const CellId &cell_id) {
  const auto *cell = std::get_if<core::CellSceneFrameRefDTO>(&frame);
  if (cell == nullptr) {
    throw SceneRequestRejectionError(
        "scene_frame_resolution_unavailable",
        "component and passive-model scene-frame resolution is installed by "
        "N5c",
        {{"reason", "scene_frame_resolver_not_installed"}});
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
                      const ContentDigest &request_digest) {
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
        if constexpr (std::is_same_v<T, core::AddObjectRequestDTO>) {
          ValidateBoundedIdentity(operation.scene_resource_id.value(),
                                  "scene_resource_id");
          static_cast<void>(
              RequireCellFrame(operation.frame_ref, current.cell_id));
          const auto found = std::lower_bound(
              candidate.snapshot.objects.begin(),
              candidate.snapshot.objects.end(), operation.scene_resource_id,
              [](const SceneObjectSnapshotDTO &object,
                 const SceneResourceId &id) {
                return object.scene_resource_id < id;
              });
          if (found != candidate.snapshot.objects.end() &&
              found->scene_resource_id == operation.scene_resource_id) {
            throw SceneRequestRejectionError(
                "object_already_exists", "scene resource already exists",
                {{"scene_resource_id", operation.scene_resource_id.value()}});
          }
          auto support_contacts = operation.support_contacts;
          for (std::size_t index = 0U; index < support_contacts.size();
               ++index) {
            ValidateAndNormalizeSupportContact(support_contacts[index], index);
          }
          std::sort(support_contacts.begin(), support_contacts.end(),
                    [](const auto &left, const auto &right) {
                      return left.support_object_id < right.support_object_id;
                    });
          for (std::size_t index = 0U; index < support_contacts.size();
               ++index) {
            const auto &contact = support_contacts[index];
            if (index != 0U && support_contacts[index - 1U].support_object_id ==
                                   contact.support_object_id) {
              RejectSupportContact(index, "duplicate_support_contact_pair",
                                   "support_object_id",
                                   "support object is declared more than once");
            }
            if (contact.support_object_id ==
                operation.scene_resource_id.value()) {
              RejectSupportContact(index, "self_support_contact",
                                   "support_object_id",
                                   "scene resource cannot support itself");
            }
            const auto support = std::lower_bound(
                current.objects.begin(), current.objects.end(),
                contact.support_object_id,
                [](const SceneObjectSnapshotDTO &object,
                   const std::string &support_id) {
                  return object.scene_resource_id.value() < support_id;
                });
            if (support == current.objects.end() ||
                support->scene_resource_id.value() !=
                    contact.support_object_id) {
              throw SceneRequestRejectionError(
                  "missing_reference",
                  "support contact names an unknown scene resource",
                  {{"reference_type", "support_scene_resource"},
                   {"reference_id", contact.support_object_id},
                   {"scene_resource_id", operation.scene_resource_id.value()}});
            }
          }
          SceneObjectSnapshotDTO object{
              operation.scene_resource_id,
              {operation.shape},
              core::CellFixedSceneObjectPlacementDTO{current.cell_id,
                                                     operation.pose},
              std::nullopt,
              std::move(support_contacts),
          };
          candidate.snapshot.objects.insert(found, object);
          candidate.operation = core::ResolvedUpsertSceneObjectDTO{object};
          candidate.resolution_evidence_digest = CellResolutionEvidence(
              request_digest, request.expected_projection_revision);
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
          static_cast<void>(
              RequireCellFrame(operation.frame_ref, current.cell_id));
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
          const bool pose_changed = !SamePose(previous_pose, operation.pose);
          const bool evidence_changed = !SameProviderEvidence(
              found->provider_evidence, operation.provider_evidence);
          found->placement = core::CellFixedSceneObjectPlacementDTO{
              current.cell_id, operation.pose};
          found->provider_evidence = operation.provider_evidence;
          candidate.operation = core::ResolvedUpsertSceneObjectDTO{*found};
          candidate.resolution_evidence_digest = CellResolutionEvidence(
              request_digest, request.expected_projection_revision);
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
          throw SceneRequestRejectionError(
              "scene_frame_resolution_unavailable",
              "carried-object attach requires the N5c immutable component "
              "frame resolver",
              {{"reason", "scene_frame_resolver_not_installed"}});
        } else if constexpr (std::is_same_v<
                                 T, core::DetachCarriedObjectRequestDTO>) {
          static_cast<void>(
              RequireCellFrame(operation.frame_ref, current.cell_id));
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
              current.cell_id, operation.pose};
          candidate.snapshot.carried_object_attachments.erase(attachment);
          candidate.operation = core::ResolvedDetachCarriedObjectDTO{
              *object, removed_id, removed_revision};
          candidate.resolution_evidence_digest = CellResolutionEvidence(
              request_digest, request.expected_projection_revision);
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
                "scene_policy_derivation_unavailable",
                "derived collision-policy allowances require the N5c retained "
                "world and attachment resolver",
                {{"reason", "collision_policy_resolver_not_installed"}});
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
      maximum < 0 ||
      static_cast<std::uint64_t>(maximum) >= kMaximumSafeIntegral) {
    throw CanonicalEmptySceneOwnerLimitError(
        "logical-scene owner event ledger is exhausted or invalid");
  }
  return static_cast<std::uint64_t>(maximum) + 1U;
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
  core::IPlannerSceneProjection *projection;
  core::ProjectionHealthDTO preflight_health;
};

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

std::vector<NamedApplyStageState> ReadNamedApplyStages(
    sqlite3 *database, const std::optional<AcceptedState> &accepted,
    const std::optional<ActiveBundlePublicationReceiptDTO> &finalization) {
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
    ++expected_lineage_generation;
  }
  if (ScalarInteger(database,
                    "SELECT count(*) FROM logical_scene_named_apply_stages") !=
          static_cast<std::int64_t>(stages.size()) ||
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

DurableState
ReadDurableState(sqlite3 *database,
                 const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query) {
  DurableState state;
  state.accepted = ReadAcceptance(database, fixed_query);
  state.rebuilt = ReadRebuild(database, state.accepted);
  state.finalization_receipt =
      ReadFinalization(database, state.accepted, state.rebuilt);
  state.named_apply_stages = ReadNamedApplyStages(database, state.accepted,
                                                  state.finalization_receipt);
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
    return state;
  }
  const auto transactions = ReadSceneTransactionStoreState(database, state);
  state.active_projection = transactions.active_projection;
  state.active_scene_snapshot = transactions.active_snapshot;
  state.projection_desynchronized = transactions.projection_desynchronized;
  state.unresolved_scene_transaction = transactions.unresolved_native_attempt;
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
      statement.Text(0) == kLogicalSceneStoreSchemaId &&
      statement.Integer(1) == kLogicalSceneStoreSchemaVersion &&
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
        "logical-scene schema differs from its exact v4 artifacts");
  }
  ValidateMetadata(database, fixed_query, admission_digest);
  ValidateMigrationMetadata(database);
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
                 0) {
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
  statement.Bind(1, kLogicalSceneStoreSchemaId);
  statement.Bind(2, kLogicalSceneStoreSchemaVersion);
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
  // Check cardinality before the DTO's canonical-binding loop. This owner
  // implements the exact zero-native production path only.
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
      !graph.runtime_units.empty() || !graph.model_instances.empty() ||
      !graph.controllable_components.empty() ||
      !graph.control_domains.empty() || !graph.control_bindings.empty() ||
      !graph.component_attachments.empty() ||
      !bundle.desired_control_configurations.empty() ||
      !bundle.capability_provider_bindings.empty() ||
      !bundle.compiled_products.viewer_model.has_value() ||
      !bundle.compiled_products.control_domain_products.empty() ||
      !bundle.compiled_products.symbol_role_maps.empty() ||
      !bundle.compiled_products.runtime_unit_products.empty() ||
      bundle.compiled_products.planning_collision_model.product_id.empty() ||
      bundle.compiled_products.planning_collision_model.product_kind !=
          core::RetainedProductKind::kPlanningCollisionModel ||
      bundle.compiled_products.planning_collision_model.product_contract_id
          .empty()) {
    throw CanonicalEmptySceneOwnerConflictError(
        "bootstrap intent does not match the fixed canonical-empty "
        "cell/bundle/graph envelope");
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
  const bool projection_available =
      !state.projection_desynchronized && !state.unresolved_scene_transaction;
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
             publication_reader, nullptr, {}) {}

  Impl(std::filesystem::path database_path,
       const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource &retained_bundle_source)
      : Impl(std::move(database_path), AuthorityFromAdmission(admission),
             publication_reader, &retained_bundle_source, {}) {}

  Impl(std::filesystem::path database_path,
       const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
       std::vector<core::IPlannerSceneProjection *> required_projections)
      : Impl(std::move(database_path), AuthorityFromAdmission(admission),
             publication_reader, &retained_bundle_source,
             std::move(required_projections)) {}

  Impl(std::filesystem::path database_path,
       AdmittedBootstrapAuthority authority,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource *retained_bundle_source,
       std::vector<core::IPlannerSceneProjection *> required_projections)
      : database_path_(std::move(database_path)),
        fixed_query_(std::move(authority.query)),
        admission_digest_(std::move(authority.admission_digest)),
        admitted_bundle_(std::move(authority.retained_bundle)),
        publication_reader_(publication_reader),
        retained_bundle_source_(retained_bundle_source),
        required_projections_(std::move(required_projections)) {
    ValidateFixedQuery(fixed_query_);
    if (required_projections_.size() > kMaximumLogicalSceneTransactions ||
        std::any_of(
            required_projections_.begin(), required_projections_.end(),
            [](const auto *projection) { return projection == nullptr; })) {
      throw std::invalid_argument(
          "required logical-scene projections must be bounded and non-null");
    }
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

  std::filesystem::path database_path_;
  ProjectionBootstrapPrerequisiteQueryDTO fixed_query_;
  ContentDigest admission_digest_;
  RetainedBundleDTO admitted_bundle_;
  const core::IActiveBundlePublicationReader &publication_reader_;
  const deployment::IVerifiedRetainedBundleSource *retained_bundle_source_;
  std::vector<core::IPlannerSceneProjection *> required_projections_;
  mutable std::mutex mutex_;
  sqlite3 *database_{};
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
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source)
    : impl_(std::make_unique<Impl>(std::move(database_path), admission,
                                   publication_reader,
                                   retained_bundle_source)) {}

CanonicalEmptySceneOwner::CanonicalEmptySceneOwner(
    std::filesystem::path database_path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
    std::vector<core::IPlannerSceneProjection *> required_projections)
    : impl_(std::make_unique<Impl>(std::move(database_path), admission,
                                   publication_reader, retained_bundle_source,
                                   std::move(required_projections))) {}

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
  auto result = StatusResult(impl_->ReadStateTransaction(), query);
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
  if (!request.native_transition_rows.empty()) {
    return RejectedStageResult(
        request, "native_projection_transition_unavailable",
        "the current I4 scene owner admits only the zero-native named Apply "
        "fixture");
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
      !predecessor->value().capability_provider_bindings.empty() ||
      !candidate->value().capability_provider_bindings.empty() ||
      predecessor->value().cell_graph.cell_id != impl_->fixed_query_.cell_id ||
      candidate->value().cell_graph.cell_id != impl_->fixed_query_.cell_id) {
    return RejectedStageResult(
        request, "named_apply_retained_bundle_mismatch",
        "the complete independently verified retained bundles do not match "
        "the request or this logical-scene lineage");
  }

  std::lock_guard lock(impl_->mutex_);
  auto *database = impl_->database();
  Transaction transaction(database, "BEGIN IMMEDIATE");
  auto state = ReadCompleteDurableState(database, impl_->fixed_query_);
  if (const auto *staged =
          state.FindNamedApply(request.apply_id, request.apply_generation);
      staged != nullptr) {
    if (staged->predecessor != request.expected_active_projection_revision ||
        staged->candidate.retained_bundle_id !=
            request.candidate_retained_bundle.retained_bundle_id ||
        staged->candidate.graph_revision !=
            request.candidate_retained_bundle.cell_graph.graph_revision) {
      transaction.Commit();
      return RejectedStageResult(
          request, "named_apply_transition_conflict",
          "another exact named Apply transition already owns the scene "
          "quiesce");
    }
    transaction.Commit();
    return StagedResult(*staged);
  }
  if (state.OpenNamedApply() != nullptr) {
    transaction.Commit();
    return RejectedStageResult(
        request, "named_apply_transition_conflict",
        "another exact named Apply transition already owns the scene "
        "quiesce");
  }
  const auto access = AccessStatus(state);
  if (!state.accepted.has_value() || !access.scene_writes_open ||
      !access.projection_leases_open || !access.active_projection.has_value() ||
      request.expected_active_projection_revision !=
          *access.active_projection ||
      request.predecessor_retained_bundle.retained_bundle_id !=
          access.active_projection->retained_bundle_id ||
      request.predecessor_retained_bundle.cell_graph.graph_revision !=
          access.active_projection->graph_revision) {
    transaction.Commit();
    return RejectedStageResult(
        request, "named_apply_predecessor_mismatch",
        "named Apply expected predecessor is not the exact open active "
        "logical-scene composite");
  }
  if (state.named_apply_stages.size() >= kMaximumNamedApplySceneStages) {
    transaction.Commit();
    return RejectedStageResult(
        request, "named_apply_stage_history_exhausted",
        "the logical-scene owner has reached its bounded named Apply "
        "history limit");
  }

  NamedApplyStageState stage{
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
  };
  InsertNamedApplyStage(database, stage);
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  const auto *persisted =
      state.FindNamedApply(stage.apply_id, stage.apply_generation);
  if (persisted == nullptr ||
      persisted->stage_generation != stage.stage_generation ||
      persisted->predecessor != stage.predecessor ||
      persisted->candidate != stage.candidate || persisted->activated ||
      persisted->discarded || persisted->finalization_receipt.has_value() ||
      AccessStatus(state).scene_writes_open ||
      AccessStatus(state).projection_leases_open) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply Stage did not durably close gates and replay exactly");
  }
  transaction.Commit();
  return StagedResult(*persisted);
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
  auto *database = impl_->database();
  Transaction transaction(database, "BEGIN IMMEDIATE");
  auto state = ReadCompleteDurableState(database, impl_->fixed_query_);
  const auto *stage =
      state.FindNamedApply(request.apply_id, request.apply_generation);
  if (stage == nullptr) {
    const auto access = AccessStatus(state);
    const bool clean_absence = state.OpenNamedApply() == nullptr &&
                               access.active_projection.has_value() &&
                               *access.active_projection ==
                                   request.expected_active_projection_revision;
    transaction.Commit();
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
  if (stage->stage_generation != request.projection_stage_generation ||
      stage->predecessor != request.expected_active_projection_revision ||
      stage->candidate != request.candidate_projection_revision) {
    transaction.Commit();
    return FailedActivationResult(
        request,
        ProjectionApplyActivationResultState::kFailedProjectionUncertain,
        "named_apply_activation_identity_mismatch",
        "the activation request does not match the exact durable stage");
  }
  if (stage->discarded) {
    transaction.Commit();
    return FailedActivationResult(
        request, ProjectionApplyActivationResultState::kStale,
        "named_apply_stage_discarded",
        "the exact named Apply stage was durably discarded");
  }
  if (stage->activated) {
    const auto result = ActivatedResult(*stage);
    transaction.Commit();
    return result;
  }
  InsertNamedApplyActivation(database, *stage);
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  stage = state.FindNamedApply(request.apply_id, request.apply_generation);
  if (stage == nullptr || !stage->activated || stage->discarded ||
      stage->stage_generation != request.projection_stage_generation ||
      stage->candidate != request.candidate_projection_revision ||
      AccessStatus(state).scene_writes_open ||
      AccessStatus(state).projection_leases_open) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply activation did not persist exactly under quiesce");
  }
  const auto result = ActivatedResult(*stage);
  transaction.Commit();
  return result;
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
  auto *database = impl_->database();
  Transaction transaction(database, "BEGIN IMMEDIATE");
  auto state = ReadCompleteDurableState(database, impl_->fixed_query_);
  const auto *stage =
      state.FindNamedApply(request.apply_id, request.apply_generation);
  if (stage == nullptr) {
    const auto access = AccessStatus(state);
    if (state.OpenNamedApply() == nullptr &&
        access.active_projection.has_value() &&
        *access.active_projection ==
            request.expected_predecessor_projection_revision) {
      const auto result =
          DiscardedResult(StageIdentityFrom(request),
                          ProjectionApplyDiscardResultState::kAlreadyAbsent);
      transaction.Commit();
      return result;
    }
    transaction.Commit();
    return FailedDiscardResult(
        request, "named_apply_discard_identity_uncertain",
        "another or contradictory transition prevents proving the exact "
        "candidate stage absent");
  }
  if (stage->stage_generation != request.projection_stage_generation ||
      stage->predecessor != request.expected_predecessor_projection_revision ||
      stage->candidate != request.candidate_projection_revision) {
    transaction.Commit();
    return FailedDiscardResult(
        request, "named_apply_discard_identity_mismatch",
        "the discard request does not match the exact durable stage");
  }
  if (stage->activated || stage->finalization_receipt.has_value()) {
    transaction.Commit();
    return FailedDiscardResult(
        request, "named_apply_discard_forbidden_after_activation",
        "the exact transition has activated and can only recover forward");
  }
  if (stage->discarded) {
    const auto result =
        DiscardedResult(*stage, ProjectionApplyDiscardResultState::kDiscarded);
    transaction.Commit();
    return result;
  }
  InsertNamedApplyDiscard(database, *stage);
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  stage = state.FindNamedApply(request.apply_id, request.apply_generation);
  const auto access = AccessStatus(state);
  if (stage == nullptr || !stage->discarded || stage->activated ||
      stage->finalization_receipt.has_value() || !access.scene_writes_open ||
      !access.projection_leases_open || !access.active_projection.has_value() ||
      *access.active_projection != stage->predecessor) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply discard did not restore the exact predecessor");
  }
  const auto result =
      DiscardedResult(*stage, ProjectionApplyDiscardResultState::kDiscarded);
  transaction.Commit();
  return result;
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
  auto state = impl_->ReadStateTransaction();
  const auto *stage =
      state.FindNamedApply(request.apply_id, request.apply_generation);
  if (stage == nullptr) {
    throw CanonicalEmptySceneOwnerConflictError(
        "named Apply Finalize has no exact durable stage");
  }
  if (stage->stage_generation != request.projection_stage_generation ||
      stage->candidate != request.candidate_projection_revision) {
    throw CanonicalEmptySceneOwnerConflictError(
        "named Apply Finalize does not match the durable stage identity");
  }
  if (stage->discarded || !stage->activated) {
    throw CanonicalEmptySceneOwnerConflictError(
        "named Apply Finalize requires the exact activated transition");
  }
  if (stage->finalization_receipt.has_value()) {
    if (*stage->finalization_receipt != request.publication_receipt) {
      throw CanonicalEmptySceneOwnerConflictError(
          "named Apply Finalize replay carries a different receipt");
    }
    return FinalizedResult(*stage);
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
    const auto &read_error_code = durable.error().code;
    const bool definitively_not_current =
        read_error_code == "missing_reference" ||
        read_error_code == "stale_generation" ||
        read_error_code == "publication_generation_not_current";
    return FailedNamedFinalizeResult(
        *stage, request,
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
        *stage, request, ProjectionApplyFinalizeResultState::kStale,
        "active_bundle_publication_receipt_mismatch",
        "the independently reread current receipt is not byte-equivalent");
  }

  auto *database = impl_->database();
  Transaction transaction(database, "BEGIN IMMEDIATE");
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  stage = state.FindNamedApply(request.apply_id, request.apply_generation);
  if (stage == nullptr || stage->discarded || !stage->activated ||
      stage->stage_generation != request.projection_stage_generation ||
      stage->candidate != request.candidate_projection_revision) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply transition changed before Finalize commit");
  }
  if (!stage->finalization_receipt.has_value()) {
    const auto owner_event_sequence = NextSceneOwnerEventSequence(database);
    const auto lineage_generation = stage->lineage_generation;
    InsertNamedApplyFinalization(database, request);
    InsertNamedApplyOwnerEvent(database, owner_event_sequence,
                               lineage_generation);
    state = ReadCompleteDurableState(database, impl_->fixed_query_);
    stage = state.FindNamedApply(request.apply_id, request.apply_generation);
  }
  const auto access = AccessStatus(state);
  if (stage == nullptr || !stage->finalization_receipt.has_value() ||
      *stage->finalization_receipt != request.publication_receipt ||
      !access.scene_writes_open || !access.projection_leases_open ||
      !access.active_projection.has_value() ||
      *access.active_projection != stage->candidate) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "named Apply Finalize did not durably open the exact candidate");
  }
  const auto result = FinalizedResult(*stage);
  transaction.Commit();
  return result;
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
  const auto status_state =
      staged->finalization_receipt.has_value()
          ? ProjectionApplyStatusState::kFinalized
      : staged->discarded ? ProjectionApplyStatusState::kDiscarded
      : staged->activated ? ProjectionApplyStatusState::kActivated
                          : ProjectionApplyStatusState::kStaged;
  const auto evidence_state =
      staged->finalization_receipt.has_value()
          ? ProjectionTransitionEvidenceState::kFinalized
      : staged->discarded ? ProjectionTransitionEvidenceState::kAborted
      : staged->activated
          ? ProjectionTransitionEvidenceState::kActivatedAndRetired
          : ProjectionTransitionEvidenceState::kStaged;
  ProjectionApplyStatusDTO status{
      staged->apply_id,
      staged->apply_generation,
      staged->predecessor,
      staged->stage_generation,
      staged->candidate,
      status_state,
      ZeroNativeTransitionEvidence(*staged, evidence_state),
      staged->finalization_receipt,
      staged->finalization_receipt.has_value()
          ? std::optional<ProjectionAggregateHealthDTO>(
                ZeroNativeNamedApplyHealth(*staged))
          : std::nullopt,
      std::nullopt,
  };
  if (!status.presence_valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "durable named Apply stage cannot form a valid status");
  }
  return status;
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

  {
    Transaction read(database, "BEGIN");
    observed_state = ReadCompleteDurableState(database, impl_->fixed_query_);
    if (!observed_state.active_projection.has_value() ||
        !observed_state.active_scene_snapshot.has_value()) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable",
                    "logical-scene bootstrap is not finalized");
    }
    const auto owner_epoch =
        observed_state.active_scene_snapshot->revision.scene_id.value();
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
      return ResultFromStoredOutcome(
          database, *existing, core::RequestReplayStatusDTO::kReplayedExact);
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
    observed_transactions =
        ReadSceneTransactionStoreState(database, observed_state);
    if (observed_transactions->next_transaction_sequence >
            kMaximumLogicalSceneTransactions ||
        observed_transactions->next_owner_event_sequence >
            kMaximumLogicalSceneOwnerEvents) {
      return reject(core::RequestReplayStatusDTO::kRequestLedgerFull,
                    "request_ledger_full",
                    "logical-scene request ledger reached its fixed bound");
    }
    try {
      candidate = ResolveSceneCandidate(
          request, *observed_state.active_scene_snapshot,
          observed_transactions->next_transaction_sequence, request_digest);
    } catch (const SceneRequestRejectionError &error) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew, error.code(),
                    error.what(), error.details());
    } catch (const CanonicalEmptySceneOwnerConflictError &error) {
      std::string_view code = "scene_object_state_conflict";
      return reject(core::RequestReplayStatusDTO::kAcceptedNew, code,
                    error.what());
    } catch (const CanonicalEmptySceneOwnerLimitError &error) {
      return reject(core::RequestReplayStatusDTO::kRequestLedgerFull,
                    "request_ledger_full", error.what());
    } catch (const std::invalid_argument &error) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "invalid_request", error.what());
    }
    read.Commit();
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

  std::vector<RequiredSceneProjection> projections;
  projections.reserve(impl_->required_projections_.size());
  for (auto *projection : impl_->required_projections_) {
    try {
      auto health = projection->ProjectionHealth();
      if (!health.valid() || health.source_sequence == 0U ||
          !health.healthy_for_target(
              scene_transaction.prior_projection_revision)) {
        return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                      "live_scene_snapshot_unavailable",
                      "a required native scene projection is not healthy at "
                      "the exact predecessor revision");
      }
      projections.push_back({projection, std::move(health)});
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
  std::sort(projections.begin(), projections.end(),
            [](const RequiredSceneProjection &left,
               const RequiredSceneProjection &right) {
              return core::CanonicalProjectionNativeBindingLess(
                  left.preflight_health.native_runtime_binding,
                  right.preflight_health.native_runtime_binding);
            });
  for (std::size_t index = 1U; index < projections.size(); ++index) {
    if (!core::CanonicalProjectionNativeBindingLess(
            projections[index - 1U].preflight_health.native_runtime_binding,
            projections[index].preflight_health.native_runtime_binding)) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "live_scene_snapshot_unavailable",
                    "required native scene projection identities are not "
                    "unique");
    }
  }

  {
    Transaction write(database, "BEGIN IMMEDIATE");
    const auto state = ReadCompleteDurableState(database, impl_->fixed_query_);
    const auto transactions = ReadSceneTransactionStoreState(database, state);
    const auto access = AccessStatus(state);
    if (!access.scene_writes_open || !state.active_projection.has_value() ||
        *state.active_projection !=
            scene_transaction.prior_projection_revision ||
        transactions.next_transaction_sequence !=
            scene_transaction.transaction_sequence ||
        transactions.next_owner_event_sequence !=
            observed_transactions->next_owner_event_sequence ||
        NextSceneOwnerEventSequence(database) !=
            observed_transactions->next_owner_event_sequence ||
        ReadOutcomeForRequest(
            database, request.request_context.request_owner_epoch.value(),
            request.request_context.request_id)
            .has_value()) {
      return reject(core::RequestReplayStatusDTO::kAcceptedNew,
                    "stale_revision",
                    "logical-scene state changed before durable acceptance");
    }
    InsertSceneIntent(database, request, request_digest, request_json,
                      scene_transaction, *candidate, projections.size());
    InsertSceneTransactionOwnerEvent(
        database, observed_transactions->next_owner_event_sequence,
        scene_transaction.transaction_sequence);
    write.Commit();
  }

  bool every_attempt_rolled_back = true;
  for (std::size_t ordinal = 0U; ordinal < projections.size(); ++ordinal) {
    const auto &projection = projections[ordinal];
    {
      Transaction attempt(database, "BEGIN IMMEDIATE");
      InsertProjectionAttempt(
          database, scene_transaction.transaction_sequence, ordinal,
          projection.preflight_health.native_runtime_binding);
      attempt.Commit();
    }

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
      const auto failed_state =
          ReadCompleteDurableState(database, impl_->fixed_query_);
      if (status ==
              SceneProjectionCommitStatusDTO::kFailedProjectionUncertain &&
          AccessStatus(failed_state).projection_leases_open) {
        throw CanonicalEmptySceneOwnerIntegrityError(
            "uncertain projection result did not close projection leases");
      }
      result_write.Commit();
      return FailedSceneResult(request.request_context.request_id,
                               core::RequestReplayStatusDTO::kAcceptedNew,
                               std::move(error), status);
    }
    result_write.Commit();
  }

  {
    Transaction outcome(database, "BEGIN IMMEDIATE");
    InsertSceneOutcome(database,
                       request.request_context.request_owner_epoch.value(),
                       request.request_context.request_id, request_digest,
                       scene_transaction.transaction_sequence, true,
                       SceneProjectionCommitStatusDTO::kCommitted,
                       projections.size(), std::nullopt);
    const auto committed_state =
        ReadCompleteDurableState(database, impl_->fixed_query_);
    if (!committed_state.active_projection.has_value() ||
        *committed_state.active_projection !=
            scene_transaction.proposed_projection_revision ||
        !AccessStatus(committed_state).scene_writes_open) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "successful scene outcome did not publish the exact composite");
    }
    outcome.Commit();
  }

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

ProjectionRevisionDTO
CanonicalEmptySceneOwner::CurrentProjectionRevision() const {
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadStateTransaction();
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
  const auto state = impl_->ReadStateTransaction();
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
  const auto snapshot = LogicalSnapshot();
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
  if (fixed == nullptr) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError("live_scene_snapshot_unavailable",
                   "carried-object cell pose requires the N5c immutable frame "
                   "projection",
                   {{"scene_resource_id", scene_resource_id.value()}}));
  }
  return core::Result<core::Pose3DTO>::Ok(fixed->pose_in_cell);
}

CanonicalEmptySceneAccessStatus
CanonicalEmptySceneOwner::ReadAccessStatus() const {
  std::lock_guard lock(impl_->mutex_);
  auto status = AccessStatus(impl_->ReadStateTransaction());
  if (!status.valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "durable state cannot form a valid access-gate status");
  }
  return status;
}

std::optional<LogicalSceneSnapshotRoot>
CanonicalEmptySceneOwner::ReadActiveSnapshot() const {
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadStateTransaction();
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
      state.projection_desynchronized || state.unresolved_scene_transaction,
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

} // namespace manyforge::scene
