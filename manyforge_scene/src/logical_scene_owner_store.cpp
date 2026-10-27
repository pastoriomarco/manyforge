#include "logical_scene_owner_impl.hpp"

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

constexpr std::int64_t kPageSizeBytes = 4096;

constexpr std::int64_t kMaximumPageCount =
    kMaximumSceneStoreDatabasePageImageBytes / kPageSizeBytes;

constexpr std::int64_t kExpectedBaseSchemaObjectCardinality = 24;

constexpr std::int64_t kExpectedTransactionSchemaObjectCardinality = 42;

constexpr std::int64_t kExpectedNativeHandoffSchemaObjectCardinality = 66;

constexpr std::int64_t kExpectedBoundedHistorySchemaObjectCardinality = 71;

constexpr std::int64_t kExpectedSchemaObjectCardinality = 60;

constexpr std::int64_t kExpectedInitialSceneSchemaObjectCardinality = 72;

constexpr std::int64_t kMaximumSchemaObjectCardinality = 80;

constexpr std::string_view kLogicalSceneTransactionStoreSchemaId =
    "manyforge.internal.logical-scene.n5b.v4";

constexpr std::string_view kMaintenanceInitializationSchemaId =
    "manyforge.internal.logical-scene.ua4.v8";

constexpr std::string_view kInitialSceneBaselineSchemaId =
    "manyforge.internal.logical-scene.ua7s.v9";

constexpr std::string_view kBoundedHistoryStoreSchemaId =
    "manyforge.internal.logical-scene.ui6i.v7";

constexpr std::string_view kNativeProjectionHandoffStoreSchemaId =
    "manyforge.internal.logical-scene.n5c.v5";

// The retired handoff store recorded this bound; migration must verify it exactly.
constexpr std::int64_t kHistoricalMaximumNativeRecoveries = 256;

constexpr int kMaximumSqliteValueBytes = 1024 * 1024;

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

[[noreturn]] void ThrowCellAdmissionFailure(
    const std::string_view context, const core::ErrorDTO &error) {
  const auto message = std::string(context) + ": " + error.message;
  if (error.code == "kernel_owner_unavailable") {
    throw CellAdmissionTemporarilyUnavailable(message);
  }
  throw LogicalSceneOwnerConflictError(message);
}

[[noreturn]] void ThrowSqlite(sqlite3 *database, const int result,
                              const std::string_view context) {
  const std::string detail =
      database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
  if (result == SQLITE_FULL || result == SQLITE_TOOBIG ||
      result == SQLITE_NOMEM) {
    throw LogicalSceneOwnerLimitError(
        std::string(context) + ": storage limit exhausted: " + detail);
  }
  if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
    throw LogicalSceneOwnerBusyError(std::string(context) + ": " +
                                            detail);
  }
  if (result == SQLITE_CONSTRAINT) {
    throw LogicalSceneOwnerConflictError(std::string(context) + ": " +
                                                detail);
  }
  if (result == SQLITE_CORRUPT || result == SQLITE_NOTADB ||
      result == SQLITE_SCHEMA) {
    throw LogicalSceneOwnerIntegrityError(std::string(context) + ": " +
                                                 detail);
  }
  throw LogicalSceneOwnerError(std::string(context) + ": " + detail);
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
      throw LogicalSceneOwnerLimitError(detail);
    }
    if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
      throw LogicalSceneOwnerBusyError(detail);
    }
    if (result == SQLITE_CONSTRAINT) {
      throw LogicalSceneOwnerConflictError(detail);
    }
    if (result == SQLITE_CORRUPT || result == SQLITE_NOTADB ||
        result == SQLITE_SCHEMA) {
      throw LogicalSceneOwnerIntegrityError(detail);
    }
    throw LogicalSceneOwnerError(detail);
  }
}

std::int64_t ScalarInteger(sqlite3 *database, const std::string_view sql) {
  Statement statement(database, sql);
  if (!statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "SQLite scalar row is missing");
  }
  const auto value = statement.Integer(0);
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "SQLite scalar query returned multiple rows");
  }
  return value;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

std::string ScalarText(sqlite3 *database, const std::string_view sql) {
  Statement statement(database, sql);
  if (!statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "SQLite scalar text row is missing");
  }
  auto value = statement.Text(0);
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
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
                "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name LIMIT 81");
  SchemaObjects objects;
  std::int64_t count = 0;
  while (statement.Step()) {
    ++count;
    if (count > kMaximumSchemaObjectCardinality) {
      throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerError(
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
    if (schema_version >= kBoundedHistoryStoreSchemaVersion) {
      Exec(database, detail::kEmbeddedBoundedHistoryMigration);
    }
    if (schema_version >= kMaintenanceInitializationSchemaVersion) {
      Exec(database, detail::kEmbeddedMaintenanceInitializationMigration);
    }
    if (schema_version >= kInitialSceneBaselineSchemaVersion) {
      Exec(database, detail::kEmbeddedInitialSceneBaselineMigration);
    }
    auto objects = ReadSchemaObjects(database);
    const auto expected_count =
        schema_version == kCanonicalEmptySceneStoreSchemaVersion
            ? kExpectedBaseSchemaObjectCardinality
        : schema_version == kLogicalSceneTransactionStoreSchemaVersion
            ? kExpectedTransactionSchemaObjectCardinality
        : schema_version == kNativeProjectionHandoffStoreSchemaVersion
            ? kExpectedNativeHandoffSchemaObjectCardinality
        : schema_version == kBoundedHistoryStoreSchemaVersion
            ? kExpectedBoundedHistorySchemaObjectCardinality
        : schema_version == kMaintenanceInitializationSchemaVersion
            ? kExpectedSchemaObjectCardinality
            : kExpectedInitialSceneSchemaObjectCardinality;
    if (objects.size() != static_cast<std::size_t>(expected_count)) {
      throw LogicalSceneOwnerError(
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

const SchemaObjects &ExpectedBoundedHistorySchemaObjects() {
  static const SchemaObjects expected =
      BuildExpectedSchemaObjects(kBoundedHistoryStoreSchemaVersion);
  return expected;
}

const SchemaObjects &ExpectedSchemaObjects() {
  static const SchemaObjects expected =
      BuildExpectedSchemaObjects(kLogicalSceneStoreSchemaVersion);
  return expected;
}

const SchemaObjects &ExpectedMaintenanceSchemaObjects() {
  static const SchemaObjects expected =
      BuildExpectedSchemaObjects(kMaintenanceInitializationSchemaVersion);
  return expected;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

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

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

void ValidateDigest(const std::string_view value,
                    const std::string_view label) {
  if (!core::IsExactLowercaseSha256Digest(value)) {
    throw std::invalid_argument(std::string(label) +
                                " must be an exact lowercase SHA-256 digest");
  }
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

RequestOwnerEpoch
CompactedRequestOwnerEpoch(const SceneId &scene_id,
                           const std::uint64_t request_epoch_generation) {
  if (scene_id.empty() || request_epoch_generation <= 1U ||
      request_epoch_generation > kMaximumSafeIntegral) {
    throw LogicalSceneOwnerIntegrityError(
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

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

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

std::string MaintenanceInitializationMigrationArtifactDigest() {
  return manyforge::content_identity::sha256_digest(
      detail::kEmbeddedMaintenanceInitializationMigration);
}

std::string InitialSceneBaselineMigrationArtifactDigest() {
  return manyforge::content_identity::sha256_digest(
      detail::kEmbeddedInitialSceneBaselineMigration);
}

std::string CurrentSchemaArtifactDigest() {
  return manyforge::content_identity::sha256_digest(
      std::string(detail::kEmbeddedCanonicalEmptySceneSchema) + "\n--v4--\n" +
      std::string(detail::kEmbeddedLogicalSceneTransactionMigration) +
      "\n--v5--\n" +
      std::string(detail::kEmbeddedNativeProjectionHandoffMigration) +
      "\n--v7--\n" + std::string(detail::kEmbeddedBoundedHistoryMigration) +
      "\n--v8--\n" +
      std::string(detail::kEmbeddedMaintenanceInitializationMigration) +
      "\n--v9--\n" +
      std::string(detail::kEmbeddedInitialSceneBaselineMigration));
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

/// Returns the exact Scene-owned current native binding set. A maintenance
/// completion supplies the complete set; only a pre-UA4 store falls back to
/// its immutable migration snapshot.
[[nodiscard]] std::vector<core::ProjectionNativeRuntimeBindingDTO>
ActiveNativeRuntimeBindings(const DurableState &state) {
  for (auto initialization = state.maintenance_initializations.rbegin();
       initialization != state.maintenance_initializations.rend();
       ++initialization) {
    if (initialization->publication_receipt.has_value()) {
      return initialization->bindings;
    }
  }
  const auto &bindings = state.active_native_runtime_bindings;
  if (!std::is_sorted(bindings.begin(), bindings.end(),
                      core::CanonicalProjectionNativeBindingLess) ||
      std::adjacent_find(bindings.begin(), bindings.end()) != bindings.end()) {
    throw LogicalSceneOwnerIntegrityError(
        "finalized native projection set is not canonical and unique");
  }
  return bindings;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

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

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

[[nodiscard]] BoundedHistoryState ReadBoundedHistoryState(sqlite3 *database) {
  if (ScalarInteger(database, "PRAGMA user_version") <
      kMaintenanceInitializationSchemaVersion) {
    return {};
  }
  Statement statement(
      database, "SELECT compaction_generation,request_epoch_generation,"
                "request_owner_epoch,compacted_through_transaction_sequence,"
                "compacted_through_owner_event_sequence,"
                "compacted_structural_completion_count,"
                "compacted_maintenance_completion_count,"
                "recovered_through_transaction_sequence,"
                "checkpoint_retained_bundle_id,"
                "checkpoint_graph_revision,checkpoint_scene_id,"
                "checkpoint_scene_revision_id,checkpoint_scene_content_hash,"
                "checkpoint_snapshot_json,"
                "compaction_in_progress FROM logical_scene_history_state "
                "ORDER BY singleton LIMIT 2");
  if (!statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "bounded logical-scene history state is missing");
  }
  const auto nonnegative = [&](const int column, const std::string_view label) {
    const auto value = statement.Integer(column);
    if (value < 0 || static_cast<std::uint64_t>(value) > kMaximumSafeIntegral) {
      throw LogicalSceneOwnerIntegrityError(
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
  const auto completion_count = nonnegative(5, "structural completion count");
  if (completion_count > kMaximumMigratedStructuralCompletions) {
    throw LogicalSceneOwnerIntegrityError(
        "bounded logical-scene structural completion count exceeds its bound");
  }
  state.compacted_structural_completion_count =
      static_cast<std::size_t>(completion_count);
  const auto maintenance_count =
      nonnegative(6, "maintenance completion count");
  if (maintenance_count > kMaximumMaintenanceInitializations) {
    throw LogicalSceneOwnerIntegrityError(
        "bounded logical-scene maintenance completion count exceeds its bound");
  }
  state.compacted_maintenance_completion_count =
      static_cast<std::size_t>(maintenance_count);
  state.recovered_through_transaction_sequence =
      nonnegative(7, "recovered transaction watermark");
  const bool checkpoint_present = !statement.IsNull(8);
  for (int column = 9; column <= 13; ++column) {
    if (statement.IsNull(column) == checkpoint_present) {
      throw LogicalSceneOwnerIntegrityError(
          "bounded logical-scene checkpoint has partial presence");
    }
  }
  if (checkpoint_present) {
    const auto revision_id = statement.Integer(11);
    if (revision_id <= 0) {
      throw LogicalSceneOwnerIntegrityError(
          "bounded logical-scene checkpoint revision is invalid");
    }
    ProjectionRevisionDTO projection{
        RetainedBundleId(statement.Text(8)), GraphRevision(statement.Text(9)),
        SceneRevision{SceneId(statement.Text(10)),
                      static_cast<std::uint64_t>(revision_id),
                      ContentDigest(statement.Text(12))}};
    auto snapshot =
        logical_scene_codec_internal::DecodeSnapshot(statement.Text(13));
    if (!projection.valid() || snapshot.revision != projection.scene_revision) {
      throw LogicalSceneOwnerIntegrityError(
          "bounded logical-scene checkpoint content is inconsistent");
    }
    state.checkpoint_projection = std::move(projection);
    state.checkpoint_snapshot = std::move(snapshot);
  }
  if (statement.Integer(14) != 0 || statement.Step() ||
      (state.request_epoch_generation == 0U) !=
          !state.request_owner_epoch.has_value() ||
      (state.compaction_generation == 0U) !=
          !state.checkpoint_projection.has_value() ||
      state.checkpoint_projection.has_value() !=
          state.checkpoint_snapshot.has_value()) {
    throw LogicalSceneOwnerIntegrityError(
        "bounded logical-scene history state violates its closed shape");
  }
  if (state.compaction_generation == 0U) {
    if (state.request_epoch_generation != 0U) {
      throw LogicalSceneOwnerIntegrityError(
          "uncompacted logical-scene history carries a request epoch");
    }
  } else {
    // Compaction keeps the epoch in force: the bootstrap epoch, or the last
    // epoch rotated by a store compacted before compaction became transparent.
    const auto &scene_id = state.checkpoint_projection->scene_revision.scene_id;
    if (state.compaction_generation >= kMaximumSafeIntegral ||
        state.request_epoch_generation < 2U ||
        state.request_epoch_generation > state.compaction_generation + 1U ||
        (*state.request_owner_epoch != RequestOwnerEpoch(scene_id.value()) &&
         *state.request_owner_epoch !=
             CompactedRequestOwnerEpoch(scene_id,
                                        state.request_epoch_generation))) {
      throw LogicalSceneOwnerIntegrityError(
          "compacted logical-scene request epoch is not deterministic");
    }
  }
  return state;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

struct StoredSceneRecord final {
  StoredSceneIntent intent;
  std::size_t projection_attempt_count;
  std::size_t projection_result_count;
  std::optional<StoredSceneOutcome> outcome;
};

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

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
  throw LogicalSceneOwnerIntegrityError(
      "stored scene projection status is invalid");
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

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
  throw LogicalSceneOwnerIntegrityError(
      "stored logical-scene update status is invalid");
}

ProjectionRevisionDTO ReadProjectionColumns(const Statement &statement,
                                            const int offset) {
  const auto revision_id = statement.Integer(offset + 3);
  if (revision_id <= 0) {
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
        "stored logical-scene projection identity is invalid");
  }
  return projection;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

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
    throw LogicalSceneOwnerIntegrityError(
        "stored scene outcome has a zero transaction sequence");
  }
  if (outcome.success) {
    if (!statement.IsNull(7) || !statement.IsNull(8) || !statement.IsNull(9)) {
      throw LogicalSceneOwnerIntegrityError(
          "successful scene outcome carries an error");
    }
  } else {
    if (statement.IsNull(7) || statement.IsNull(8) || statement.IsNull(9)) {
      throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
        "stored scene outcome violates its closed result shape");
  }
  return outcome;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

std::optional<StoredSceneOutcome>
ReadOutcomeForSequence(sqlite3 *database,
                       const std::uint64_t transaction_sequence) {
  Statement key(database, "SELECT request_owner_epoch,request_id FROM "
                          "logical_scene_transaction_intents WHERE "
                          "transaction_sequence=? LIMIT 2");
  key.Bind(1, CheckedSafeCounter(transaction_sequence, "transaction_sequence"));
  if (!key.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "scene outcome lookup has no matching intent");
  }
  const auto epoch = key.Text(0);
  const RequestId request_id(key.Text(1));
  if (key.Step()) {
    throw LogicalSceneOwnerIntegrityError(
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
      throw LogicalSceneOwnerIntegrityError(
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
      throw LogicalSceneOwnerIntegrityError(
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
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene projection result ordinals are invalid");
    }
    const auto state = results.Text(1);
    const bool acknowledgement_present = !results.IsNull(2);
    for (int column = 2; column <= 6; ++column) {
      if (results.IsNull(column) == acknowledgement_present) {
        throw LogicalSceneOwnerIntegrityError(
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
      throw LogicalSceneOwnerIntegrityError(
          "stored projection result error has partial presence");
    }
    if (error_present) {
      static_cast<void>(
          logical_scene_codec_internal::DecodeErrorDetails(results.Text(11)));
    }
    if (state == "committed") {
      if (!acknowledgement.has_value() || *acknowledgement != proposed ||
          rollback_proven || health_state != "healthy" || error_present) {
        throw LogicalSceneOwnerIntegrityError(
            "stored committed projection result is contradictory");
      }
      evidence.any_result_committed = true;
      evidence.all_results_rollback_proven = false;
    } else if (state == "rejected_rollback_proven") {
      if (!rollback_proven || health_state != "healthy" || !error_present) {
        throw LogicalSceneOwnerIntegrityError(
            "stored rollback-proven projection result is contradictory");
      }
      evidence.all_results_committed = false;
    } else if (state == "failed_projection_uncertain") {
      if (rollback_proven ||
          (health_state != "desynchronized" && health_state != "unavailable") ||
          !error_present) {
        throw LogicalSceneOwnerIntegrityError(
            "stored uncertain projection result is contradictory");
      }
      evidence.all_results_committed = false;
      evidence.all_results_rollback_proven = false;
      evidence.any_result_uncertain = true;
    } else {
      throw LogicalSceneOwnerIntegrityError(
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

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

SceneTransactionStoreState
ReadSceneTransactionStoreState(sqlite3 *database,
                               const DurableState &durable_state) {
  if (!durable_state.accepted.has_value() ||
      !durable_state.finalization_receipt.has_value()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene transaction history requires finalized bootstrap");
  }
  const auto &bootstrap_projection = durable_state.accepted->target;
  const auto &bootstrap_snapshot = durable_state.accepted->snapshot;
  const auto history = ReadBoundedHistoryState(database);
  if (history.compacted_structural_completion_count >
      durable_state.migrated_structural_completions.size()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene checkpoint names absent structural history");
  }
  std::vector<const MaintenanceInitializationState *> completed_maintenance;
  for (const auto &initialization :
       durable_state.maintenance_initializations) {
    if (initialization.publication_receipt.has_value()) {
      completed_maintenance.push_back(&initialization);
    }
  }
  if (history.compacted_maintenance_completion_count >
      completed_maintenance.size()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene checkpoint names absent maintenance history");
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
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene checkpoint is outside the accepted lineage");
    }
  }
  const RequestOwnerEpoch request_owner_epoch =
      history.request_owner_epoch.value_or(
          RequestOwnerEpoch(bootstrap_snapshot.revision.scene_id.value()));
  if (request_owner_epoch.empty() ||
      history.compacted_through_transaction_sequence >= kMaximumSafeIntegral ||
      history.compacted_through_owner_event_sequence >= kMaximumSafeIntegral) {
    throw LogicalSceneOwnerIntegrityError(
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
  std::size_t next_maintenance_completion =
      history.compacted_maintenance_completion_count;
  const auto apply_maintenance_until =
      [&](const std::optional<ProjectionRevisionDTO> &expected) {
        while (next_maintenance_completion < completed_maintenance.size()) {
          const auto &initialization =
              *completed_maintenance[next_maintenance_completion];
          // A configuration-only Apply may leave the composite unchanged.
          // Consume those completions before the next Scene transaction, but
          // not across a no-op transaction that kept the same revision.
          if (expected.has_value() && state.active_projection == *expected &&
              !(initialization.predecessor == state.active_projection &&
                initialization.target == state.active_projection &&
                initialization.authoritative_snapshot.transaction_sequence ==
                    state.active_snapshot.transaction_sequence)) {
            break;
          }
          if (initialization.predecessor != state.active_projection ||
              initialization.predecessor.scene_revision !=
                  state.active_snapshot.revision ||
              initialization.authoritative_snapshot.revision !=
                  initialization.target.scene_revision ||
              initialization.authoritative_snapshot.cell_id !=
                  state.active_snapshot.cell_id ||
              initialization.authoritative_snapshot.transaction_sequence !=
                  state.active_snapshot.transaction_sequence ||
              (initialization.initial_baseline_request_digest.has_value() &&
               !initialization.initial_baseline_installed)) {
            throw LogicalSceneOwnerIntegrityError(
                "maintenance completion does not follow the authoritative "
                "Scene head");
          }
          state.active_projection = initialization.target;
          state.active_snapshot = initialization.authoritative_snapshot;
          ++next_maintenance_completion;
        }
        return !expected.has_value() || state.active_projection == *expected;
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
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene history continues after an unresolved or "
          "desynchronized transaction");
    }
    const auto sequence_value = statement.Integer(0);
    if (sequence_value <= 0 ||
        static_cast<std::uint64_t>(sequence_value) != expected_sequence) {
      throw LogicalSceneOwnerIntegrityError(
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
        !ExpectedProjectionAdmitsActive(stored_request, intent.prior) ||
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
      throw LogicalSceneOwnerIntegrityError(
          "durable logical-scene transaction intent is invalid");
    }
    const auto projection_evidence = ReadProjectionEvidenceState(
        database, expected_sequence, intent.proposed);
    const auto attempts = projection_evidence.attempt_count;
    const auto results = projection_evidence.result_count;
    if (attempts > intent.required_projection_count || results > attempts) {
      throw LogicalSceneOwnerIntegrityError(
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
        throw LogicalSceneOwnerIntegrityError(
            "logical-scene outcome does not match its durable intent");
      }
      if (outcome->success && attempts != intent.required_projection_count) {
        throw LogicalSceneOwnerIntegrityError(
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
        throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerLimitError(
        "logical-scene transaction ledger exceeds its fixed bound");
  }
  if (history.recovered_through_transaction_sequence >=
      state.next_transaction_sequence) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene recovery watermark exceeds durable transaction "
        "history");
  }

  std::vector<bool> transaction_event_seen(records.size(), false);
  std::vector<bool> structural_completion_event_seen(
      durable_state.migrated_structural_completions.size(), false);
  Statement events(
      database,
      "SELECT owner_event_sequence,event_kind,transaction_sequence,"
      "structural_completion_sequence FROM logical_scene_owner_events "
      "WHERE owner_event_sequence>? ORDER BY owner_event_sequence LIMIT 513");
  events.Bind(1, static_cast<std::int64_t>(
                     history.compacted_through_owner_event_sequence));
  std::uint64_t expected_event_sequence = state.next_owner_event_sequence;
  std::size_t resident_event_count = 0U;
  bool history_sealed = false;
  while (events.Step()) {
    if (history_sealed) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene owner history continues after an unresolved or "
          "desynchronized event");
    }
    if (++resident_event_count > kMaximumLogicalSceneOwnerEvents ||
        expected_event_sequence > kMaximumSafeIntegral ||
        events.Integer(0) !=
            static_cast<std::int64_t>(expected_event_sequence)) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene owner event sequence is invalid");
    }
    const auto kind = events.Text(1);
    if (kind == "scene_transaction_intent") {
      if (events.IsNull(2) || !events.IsNull(3)) {
        throw LogicalSceneOwnerIntegrityError(
            "scene transaction owner event has invalid referent presence");
      }
      const auto sequence = events.Integer(2);
      if (sequence <= 0 ||
          static_cast<std::uint64_t>(sequence) <=
              history.compacted_through_transaction_sequence ||
          static_cast<std::uint64_t>(sequence) >= expected_sequence) {
        throw LogicalSceneOwnerIntegrityError(
            "scene transaction owner event references an absent intent");
      }
      const auto index = static_cast<std::size_t>(
          static_cast<std::uint64_t>(sequence) -
          history.compacted_through_transaction_sequence - 1U);
      if (index >= records.size()) {
        throw LogicalSceneOwnerIntegrityError(
            "scene transaction owner event exceeds resident history");
      }
      auto &record = records[index];
      if (transaction_event_seen[index] ||
          !apply_maintenance_until(record.intent.prior)) {
        throw LogicalSceneOwnerIntegrityError(
            "scene transaction event does not follow the active composite");
      }
      transaction_event_seen[index] = true;
      if (!record.outcome.has_value()) {
        history_sealed = true;
      } else if (record.outcome->success) {
        state.active_projection = record.intent.proposed;
        state.active_snapshot = record.intent.candidate_snapshot;
      } else if (record.outcome->projection_status ==
                     SceneProjectionCommitStatusDTO::
                         kFailedProjectionUncertain &&
                 record.intent.transaction_sequence >
                     history.recovered_through_transaction_sequence) {
        history_sealed = true;
      }
    } else if (kind == "migrated_structural_completion") {
      if (!events.IsNull(2) || events.IsNull(3)) {
        throw LogicalSceneOwnerIntegrityError(
          "migrated structural event has invalid referent presence");
      }
      const auto lineage = events.Integer(3);
      const auto completion = std::ranges::find_if(
          durable_state.migrated_structural_completions,
          [lineage](const MigratedStructuralCompletionState &candidate) {
            return lineage > 0 && candidate.completion_sequence ==
                                      static_cast<std::uint64_t>(lineage);
          });
      if (completion == durable_state.migrated_structural_completions.end()) {
        throw LogicalSceneOwnerIntegrityError(
            "migrated structural event references an absent completion");
      }
      const auto index = static_cast<std::size_t>(std::distance(
          durable_state.migrated_structural_completions.begin(), completion));
      if (index < history.compacted_structural_completion_count) {
        throw LogicalSceneOwnerIntegrityError(
            "migrated structural event repeats compacted history");
      }
      if (structural_completion_event_seen[index]) {
        throw LogicalSceneOwnerIntegrityError(
            "migrated structural completion event is duplicated");
      }
      structural_completion_event_seen[index] = true;
      state.active_projection = completion->target;
      if (state.active_snapshot.revision !=
          state.active_projection.scene_revision) {
        throw LogicalSceneOwnerIntegrityError(
            "migrated structural event changes logical-scene content");
      }
    } else {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene owner event kind is invalid");
    }
    ++expected_event_sequence;
  }
  state.next_owner_event_sequence = expected_event_sequence;

  for (std::size_t index = 0U; index < records.size(); ++index) {
    if (!transaction_event_seen[index]) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene intent lacks its owner-order event");
    }
  }
  for (std::size_t index = 0U;
       index < durable_state.migrated_structural_completions.size();
       ++index) {
    if (index < history.compacted_structural_completion_count) {
      continue;
    }
    if (!structural_completion_event_seen[index]) {
      throw LogicalSceneOwnerIntegrityError(
          "migrated structural completion lacks its owner-order event");
    }
  }
  static_cast<void>(apply_maintenance_until(std::nullopt));
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
    throw LogicalSceneOwnerIntegrityError(
        "typed structural head disagrees with owner-event replay");
  }
  return state;
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
    throw LogicalSceneOwnerIntegrityError(
        "scene outcome insert unexpectedly returned a row");
  }
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

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
    throw LogicalSceneOwnerIntegrityError(
        "scene result references an absent transaction intent");
  }
  auto result = ReadProjectionColumns(statement, 0);
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
        "scene result update status is absent");
  }
  const auto result = DecodeUpdateStatus(statement.Text(0));
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "scene result update status is not unique");
  }
  return result;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

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
    throw LogicalSceneOwnerIntegrityError(
        "stored scene outcome cannot form a valid public result");
  }
  return result;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

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
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
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
      throw LogicalSceneOwnerIntegrityError(
          "stored canonical-empty scene identities do not match their "
          "qualified preimages");
    }
  } catch (const LogicalSceneOwnerIntegrityError &) {
    throw;
  } catch (const std::exception &error) {
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene rebuild cardinality exceeds one");
  }
  if (!accepted.has_value() || bootstrap_id != accepted->bootstrap_id.value() ||
      bootstrap_generation !=
          static_cast<std::int64_t>(accepted->bootstrap_generation.value()) ||
      rebuild_count != 0 || health_state != "healthy" || health_count != 0) {
    throw LogicalSceneOwnerIntegrityError(
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
  const auto compatibility_materialization_digest = statement.Text(13);
  const auto public_pointer_digest = statement.Text(14);
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
        "stored finalization receipt does not match its accepted target");
  }
  ValidateDigest(private_materialization_digest,
                 "stored private materialization digest");
  ValidateDigest(compatibility_materialization_digest,
                 "stored compatibility materialization digest");
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
    throw LogicalSceneOwnerIntegrityError(
        "stored finalization receipt is structurally invalid");
  }
  return receipt;
}

std::vector<MigratedStructuralCompletionState>
ReadMigratedStructuralCompletions(sqlite3 *database) {
  Statement statement(
      database,
      "SELECT completion_sequence,apply_id,apply_generation,"
      "target_retained_bundle_id,target_graph_revision,target_scene_id,"
      "target_scene_revision_id,target_scene_content_hash FROM "
      "logical_scene_migrated_structural_completions "
      "ORDER BY completion_sequence LIMIT 257");
  std::vector<MigratedStructuralCompletionState> completions;
  std::uint64_t previous_sequence = 0U;
  while (statement.Step()) {
    const auto sequence = statement.Integer(0);
    const auto generation = statement.Integer(2);
    const auto scene_revision = statement.Integer(6);
    if (sequence <= 0 || generation <= 0 || scene_revision <= 0 ||
        static_cast<std::uint64_t>(sequence) <= previous_sequence ||
        completions.size() >= kMaximumMigratedStructuralCompletions) {
      throw LogicalSceneOwnerIntegrityError(
          "migrated structural completion is invalid");
    }
    MigratedStructuralCompletionState completion{
        static_cast<std::uint64_t>(sequence),
        ApplyId(statement.Text(1)),
        ApplyGeneration(static_cast<std::uint64_t>(generation)),
        ProjectionRevisionDTO{
            RetainedBundleId(statement.Text(3)),
            GraphRevision(statement.Text(4)),
            SceneRevision{SceneId(statement.Text(5)),
                          static_cast<std::uint64_t>(scene_revision),
                          ContentDigest(statement.Text(7))}}};
    if (completion.apply_id.empty() || !completion.target.valid()) {
      throw LogicalSceneOwnerIntegrityError(
          "migrated structural completion identity is invalid");
    }
    previous_sequence = completion.completion_sequence;
    completions.push_back(std::move(completion));
  }
  return completions;
}

[[nodiscard]] std::vector<core::ProjectionNativeRuntimeBindingDTO>
ReadActiveNativeRuntimeBindings(sqlite3 *database) {
  Statement statement(
      database,
      "SELECT binding_ordinal,provider_binding_id,"
      "provider_binding_revision,runtime_unit_id,runtime_epoch FROM "
      "logical_scene_migrated_active_native_runtime_bindings "
      "ORDER BY binding_ordinal LIMIT 257");
  std::vector<core::ProjectionNativeRuntimeBindingDTO> bindings;
  while (statement.Step()) {
    if (bindings.size() >= kMaximumNativeProjectionBindings ||
        statement.Integer(0) != static_cast<std::int64_t>(bindings.size())) {
      throw LogicalSceneOwnerIntegrityError(
          "active native binding order or cardinality is invalid");
    }
    const auto provider_binding_id = statement.Text(1);
    const auto provider_binding_revision = statement.Text(2);
    const auto runtime_unit_id = statement.Text(3);
    const auto runtime_epoch = statement.Text(4);
    ValidateBoundedIdentity(provider_binding_id,
                            "stored active provider binding ID");
    ValidateBoundedIdentity(provider_binding_revision,
                            "stored active provider binding revision");
    ValidateBoundedIdentity(runtime_unit_id,
                            "stored active runtime unit ID");
    ValidateBoundedIdentity(runtime_epoch, "stored active runtime epoch");
    bindings.push_back({
        core::CapabilityProviderBindingId(provider_binding_id),
        core::CapabilityProviderBindingRevision(provider_binding_revision),
        core::RuntimeUnitId(runtime_unit_id),
        core::RuntimeEpoch(runtime_epoch),
    });
  }
  if (!std::is_sorted(bindings.begin(), bindings.end(),
                      core::CanonicalProjectionNativeBindingLess) ||
      std::adjacent_find(bindings.begin(), bindings.end()) != bindings.end()) {
    throw LogicalSceneOwnerIntegrityError(
        "active native bindings are not exact and canonical");
  }
  return bindings;
}

[[nodiscard]] bool
ReadLegacyNativeEffectsUncertain(sqlite3 *database) {
  Statement statement(
      database,
      "SELECT legacy_native_effects_uncertain FROM "
      "logical_scene_maintenance_migration_state WHERE singleton=1 LIMIT 2");
  if (!statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "projection maintenance current state is missing");
  }
  const auto uncertain = statement.Integer(0);
  if ((uncertain != 0 && uncertain != 1) || statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "projection maintenance current state is malformed");
  }
  return uncertain != 0;
}

std::vector<MaintenanceInitializationState>
ReadMaintenanceInitializations(sqlite3 *database,
                               const CellId &expected_cell_id) {
  Statement intents(
      database,
      "SELECT initialization_sequence,apply_id,apply_generation,"
      "predecessor_retained_bundle_id,predecessor_graph_revision,"
      "predecessor_scene_id,predecessor_scene_revision_id,"
      "predecessor_scene_content_hash,target_retained_bundle_id,"
      "target_graph_revision,target_scene_id,target_scene_revision_id,"
      "target_scene_content_hash,authoritative_snapshot_json,"
      "native_binding_count,repaired_transaction_sequence "
      "FROM logical_scene_maintenance_initialization_intents "
      "ORDER BY initialization_sequence LIMIT 257");
  std::vector<MaintenanceInitializationState> result;
  while (intents.Step()) {
    if (result.size() >= kMaximumMaintenanceInitializations) {
      throw LogicalSceneOwnerIntegrityError(
          "maintenance initialization history exceeds its bound");
    }
    const auto sequence = intents.Integer(0);
    const auto generation = intents.Integer(2);
    const auto predecessor_revision = intents.Integer(6);
    const auto target_revision = intents.Integer(11);
    const auto binding_count = intents.Integer(14);
    if (sequence <= 0 || generation <= 0 || predecessor_revision <= 0 ||
        target_revision <= 0 || binding_count < 0 || binding_count > 256 ||
        sequence != static_cast<std::int64_t>(result.size() + 1U)) {
      throw LogicalSceneOwnerIntegrityError(
          "maintenance initialization intent is structurally invalid");
    }
    MaintenanceInitializationState state{
        static_cast<std::uint64_t>(sequence),
        ApplyId(intents.Text(1)),
        ApplyGeneration(static_cast<std::uint64_t>(generation)),
        ProjectionRevisionDTO{
            RetainedBundleId(intents.Text(3)), GraphRevision(intents.Text(4)),
            SceneRevision{SceneId(intents.Text(5)),
                          static_cast<std::uint64_t>(predecessor_revision),
                          ContentDigest(intents.Text(7))}},
        ProjectionRevisionDTO{
            RetainedBundleId(intents.Text(8)), GraphRevision(intents.Text(9)),
            SceneRevision{SceneId(intents.Text(10)),
                          static_cast<std::uint64_t>(target_revision),
                          ContentDigest(intents.Text(12))}},
        logical_scene_codec_internal::DecodeSnapshot(intents.Text(13)),
        {},
        std::nullopt,
        std::nullopt,
        false,
        std::nullopt,
        intents.IsNull(15)
            ? std::nullopt
            : std::optional<std::uint64_t>{static_cast<std::uint64_t>(
                  intents.Integer(15))},
        std::nullopt,
        true,
        std::nullopt,
        false,
    };
    if (state.apply_id.empty() || !state.apply_generation.nonzero() ||
        !state.predecessor.valid() || !state.target.valid() ||
        state.authoritative_snapshot.cell_id != expected_cell_id ||
        state.authoritative_snapshot.revision != state.target.scene_revision) {
      throw LogicalSceneOwnerIntegrityError(
          "maintenance initialization identity is inconsistent");
    }

    Statement bindings(
        database,
        "SELECT binding_ordinal,provider_binding_id,"
        "provider_binding_revision,runtime_unit_id,runtime_epoch FROM "
        "logical_scene_maintenance_initialization_bindings WHERE "
        "initialization_sequence=? ORDER BY binding_ordinal LIMIT 257");
    bindings.Bind(1, sequence);
    while (bindings.Step()) {
      if (bindings.Integer(0) !=
          static_cast<std::int64_t>(state.bindings.size())) {
        throw LogicalSceneOwnerIntegrityError(
            "maintenance initialization binding order is invalid");
      }
      state.bindings.push_back({
          core::CapabilityProviderBindingId(bindings.Text(1)),
          core::CapabilityProviderBindingRevision(bindings.Text(2)),
          core::RuntimeUnitId(bindings.Text(3)),
          core::RuntimeEpoch(bindings.Text(4)),
      });
    }
    if (state.bindings.size() != static_cast<std::size_t>(binding_count) ||
        !std::is_sorted(state.bindings.begin(), state.bindings.end(),
                        core::CanonicalProjectionNativeBindingLess)) {
      throw LogicalSceneOwnerIntegrityError(
          "maintenance initialization binding set is inconsistent");
    }

    if (ScalarInteger(database, "PRAGMA user_version") >=
        kInitialSceneBaselineSchemaVersion) {
      Statement baseline_request(
          database,
          "SELECT request_digest FROM logical_scene_initial_baseline_requests "
          "WHERE initialization_sequence=? LIMIT 2");
      baseline_request.Bind(1, sequence);
      if (baseline_request.Step()) {
        state.initial_baseline_request_digest =
            ContentDigest(baseline_request.Text(0));
        state.initial_baseline_installed = false;
        if (!core::IsExactLowercaseSha256Digest(
                state.initial_baseline_request_digest->value()) ||
            baseline_request.Step()) {
          throw LogicalSceneOwnerIntegrityError(
              "initial Scene request digest is invalid");
        }
        Statement baseline_intent(
            database,
            "SELECT target_retained_bundle_id,target_graph_revision,"
            "target_scene_id,target_scene_revision_id,"
            "target_scene_content_hash,snapshot_json FROM "
            "logical_scene_initial_baseline_intents WHERE "
            "initialization_sequence=? LIMIT 2");
        baseline_intent.Bind(1, sequence);
        std::optional<ProjectionRevisionDTO> final_target;
        std::optional<LogicalSceneSnapshotDTO> final_snapshot;
        if (baseline_intent.Step()) {
          const auto revision_id = baseline_intent.Integer(3);
          if (revision_id <= 0) {
            throw LogicalSceneOwnerIntegrityError(
                "initial Scene baseline revision is invalid");
          }
          final_target = ProjectionRevisionDTO{
              RetainedBundleId(baseline_intent.Text(0)),
              GraphRevision(baseline_intent.Text(1)),
              SceneRevision{SceneId(baseline_intent.Text(2)),
                            static_cast<std::uint64_t>(revision_id),
                            ContentDigest(baseline_intent.Text(4))}};
          final_snapshot = logical_scene_codec_internal::DecodeSnapshot(
              baseline_intent.Text(5));
          if (!final_target->valid() ||
              final_target->retained_bundle_id != state.target.retained_bundle_id ||
              final_target->graph_revision != state.target.graph_revision ||
              final_target->scene_revision.scene_id !=
                  state.target.scene_revision.scene_id ||
              final_target->scene_revision.revision_id <=
                  state.target.scene_revision.revision_id ||
              final_snapshot->revision != final_target->scene_revision ||
              final_snapshot->cell_id != expected_cell_id ||
              core::logical_scene_identity::ComputeLogicalSceneContentHash(
                  *final_snapshot) != final_target->scene_revision.content_hash ||
              baseline_intent.Step()) {
            throw LogicalSceneOwnerIntegrityError(
                "initial Scene baseline target is not an exact successor");
          }
        }
        Statement baseline_result(
            database,
            "SELECT state,first_error_code,first_error_message FROM "
            "logical_scene_initial_baseline_results WHERE "
            "initialization_sequence=? LIMIT 2");
        baseline_result.Bind(1, sequence);
        if (baseline_result.Step()) {
          const auto result_state = baseline_result.Text(0);
          if (result_state == "installed" && final_target.has_value() &&
              baseline_result.IsNull(1) && baseline_result.IsNull(2)) {
            state.target = *final_target;
            state.authoritative_snapshot = *final_snapshot;
            state.initial_baseline_installed = true;
          } else if ((result_state == "failed" ||
                      result_state == "interrupted") &&
                     !baseline_result.IsNull(1) &&
                     !baseline_result.IsNull(2)) {
            state.initial_baseline_error = ErrorDTO{
                baseline_result.Text(1), baseline_result.Text(2), {}};
            state.initial_baseline_effects_unknown =
                result_state == "interrupted";
          } else {
            throw LogicalSceneOwnerIntegrityError(
                "initial Scene baseline result is contradictory");
          }
          if (baseline_result.Step()) {
            throw LogicalSceneOwnerIntegrityError(
                "initial Scene baseline result is duplicated");
          }
        }
      }
    }

    Statement outcome(
        database,
        "SELECT state,first_error_code,first_error_message,effects_unknown "
        "FROM logical_scene_maintenance_initialization_results WHERE "
        "initialization_sequence=? LIMIT 2");
    outcome.Bind(1, sequence);
    if (outcome.Step()) {
      const auto result_state = outcome.Text(0);
      if (result_state == "initialized") {
        state.result_state =
            ProjectionMaintenanceInitializationState::kInitialized;
      } else if (result_state == "failed") {
        state.result_state = ProjectionMaintenanceInitializationState::kFailed;
      } else if (result_state == "interrupted") {
        state.result_state =
            ProjectionMaintenanceInitializationState::kInterrupted;
      } else {
        throw LogicalSceneOwnerIntegrityError(
            "maintenance initialization result state is invalid");
      }
      state.effects_unknown = outcome.Integer(3) == 1;
      if (!outcome.IsNull(1)) {
        state.first_error = ErrorDTO{outcome.Text(1), outcome.Text(2), {}};
      }
      if (outcome.Step()) {
        throw LogicalSceneOwnerIntegrityError(
            "maintenance initialization has duplicate results");
      }
    }

    // A baseline-specific interrupted result dominates the provisional
    // projection result; no completion may subsequently publish it.
    if (state.initial_baseline_error.has_value()) {
      state.result_state =
          state.initial_baseline_effects_unknown
              ? ProjectionMaintenanceInitializationState::kInterrupted
              : ProjectionMaintenanceInitializationState::kFailed;
      state.first_error = state.initial_baseline_error;
      state.effects_unknown = state.initial_baseline_effects_unknown;
    }

    Statement completion(
        database,
        "SELECT expected_previous_publication_generation,"
        "publication_generation,private_materialization_content_digest,"
        "durable_public_pointer_content_digest FROM "
        "logical_scene_maintenance_initialization_completions WHERE "
        "initialization_sequence=? LIMIT 2");
    completion.Bind(1, sequence);
    if (completion.Step()) {
      const auto previous = completion.Integer(0);
      const auto published = completion.Integer(1);
      if (previous <= 0 || published <= 0) {
        throw LogicalSceneOwnerIntegrityError(
            "maintenance completion publication generation is invalid");
      }
      state.publication_receipt = ActiveBundlePublicationReceiptDTO{
          ActiveBundlePublicationRequestDTO{
              NamedApplyPublicationTransactionDTO{
                  state.apply_id, state.apply_generation},
              state.target,
              state.target.retained_bundle_id,
              state.target.graph_revision,
              ActiveBundlePublicationGeneration(
                  static_cast<std::uint64_t>(previous)),
              ContentDigest(completion.Text(2)),
          },
          ActiveBundlePublicationGeneration(
              static_cast<std::uint64_t>(published)),
          ContentDigest(completion.Text(3)),
      };
      state.result_state =
          ProjectionMaintenanceInitializationState::kCompleted;
      if (!state.publication_receipt->valid() || completion.Step()) {
        throw LogicalSceneOwnerIntegrityError(
            "maintenance completion receipt is invalid");
      }
    }
    result.push_back(std::move(state));
  }
  return result;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

bool SettleInterruptedMaintenanceInitializations(sqlite3 *database) {
  Transaction settlement(database, "BEGIN IMMEDIATE");
  Statement initialized(
      database,
      "UPDATE logical_scene_maintenance_initialization_results SET "
      "state='interrupted',"
      "first_error_code='scene_initialization_interrupted',"
      "first_error_message='Scene owner reopened before maintenance completion',"
      "effects_unknown=1 WHERE state='initialized' AND NOT EXISTS(SELECT 1 "
      "FROM logical_scene_maintenance_initialization_completions AS c WHERE "
      "c.initialization_sequence="
      "logical_scene_maintenance_initialization_results.initialization_sequence)");
  if (initialized.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "initialized maintenance settlement returned a row");
  }
  const auto initialized_changes = sqlite3_changes(database);
  Statement interrupted(
      database,
      "INSERT INTO logical_scene_maintenance_initialization_results("
      "initialization_sequence,state,first_error_code,first_error_message,"
      "effects_unknown) SELECT i.initialization_sequence,'interrupted',"
      "'scene_initialization_interrupted',"
      "'Scene owner reopened before maintenance completion',"
      "1 FROM logical_scene_maintenance_initialization_intents AS i "
      "LEFT JOIN logical_scene_maintenance_initialization_results AS r "
      "USING(initialization_sequence) WHERE r.initialization_sequence IS NULL");
  if (interrupted.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "interrupted maintenance settlement returned a row");
  }
  const auto intent_changes = sqlite3_changes(database);
  settlement.Commit();
  return initialized_changes != 0 || intent_changes != 0;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

DurableState
ReadDurableState(sqlite3 *database,
                 const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query) {
  DurableState state;
  state.accepted = ReadAcceptance(database, fixed_query);
  state.rebuilt = ReadRebuild(database, state.accepted);
  state.finalization_receipt =
      ReadFinalization(database, state.accepted, state.rebuilt);
  if (ScalarInteger(database, "PRAGMA user_version") >=
      kMaintenanceInitializationSchemaVersion) {
    state.migrated_structural_completions =
        ReadMigratedStructuralCompletions(database);
    state.maintenance_initializations =
        ReadMaintenanceInitializations(database, fixed_query.cell_id);
    state.active_native_runtime_bindings =
        ReadActiveNativeRuntimeBindings(database);
    state.legacy_native_effects_uncertain =
        ReadLegacyNativeEffectsUncertain(database);
  }
  if (state.accepted.has_value() && state.finalization_receipt.has_value()) {
    state.active_projection = state.accepted->target;
    state.active_scene_snapshot = state.accepted->snapshot;
    for (const auto &completion : state.migrated_structural_completions) {
      state.active_projection = completion.target;
    }
    for (const auto &initialization : state.maintenance_initializations) {
      if (initialization.publication_receipt.has_value()) {
        state.active_projection = initialization.target;
      }
    }
  }
  return state;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

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
  for (const auto &initialization : state.maintenance_initializations) {
    if (initialization.publication_receipt.has_value()) {
      state.active_projection = ProjectionRevisionDTO{
          initialization.target.retained_bundle_id,
          initialization.target.graph_revision,
          state.active_scene_snapshot->revision};
    }
  }
  state.projection_desynchronized = transactions.projection_desynchronized;
  state.unresolved_scene_transaction = transactions.unresolved_native_attempt;
  state.latest_uncertain_transaction_sequence =
      transactions.latest_uncertain_transaction_sequence;
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
    throw LogicalSceneOwnerIntegrityError(
        "unresolved scene transaction intent is absent");
  }
  const auto request_owner_epoch = intent.Text(0);
  const RequestId request_id(intent.Text(1));
  const ContentDigest request_digest(intent.Text(2));
  const auto required_count_value = intent.Integer(3);
  const auto proposed = ReadProjectionColumns(intent, 4);
  if (intent.Step() || required_count_value < 0 || required_count_value > 256) {
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
        "scene transaction recovery did not settle the durable owner state");
  }
  recovery.Commit();
  return true;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

void ValidateMetadata(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  const auto completion_bound_column =
      ScalarInteger(database, "PRAGMA user_version") >=
              kMaintenanceInitializationSchemaVersion
          ? "maximum_migrated_structural_completions"
          : "maximum_named_apply_stages";
  Statement statement(
      database,
      std::string("SELECT schema_id,schema_version,schema_artifact_digest,") +
          "canonical_empty_admission_digest,cell_id,"
          "canonical_empty_retained_bundle_id,canonical_empty_graph_revision,"
          "maximum_identifier_bytes,maximum_database_page_image_bytes,"
          "maximum_wal_bytes," + completion_bound_column +
          " FROM logical_scene_store_metadata ORDER BY singleton LIMIT 2");
  if (!statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
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
  const auto maximum_structural_completions = statement.Integer(10);
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
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
      maximum_structural_completions !=
          static_cast<std::int64_t>(kMaximumMigratedStructuralCompletions)) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene metadata does not match the fixed v2 owner");
  }
}

void ValidateMechanicalDatabaseIntegrity(sqlite3 *database) {
  if (ScalarText(database, "PRAGMA quick_check(1)") != "ok") {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene SQLite quick_check failed");
  }
  Statement foreign_keys(database, "PRAGMA foreign_key_check");
  if (foreign_keys.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene SQLite foreign-key check failed");
  }
  const auto page_count = ScalarInteger(database, "PRAGMA page_count");
  const auto page_size = ScalarInteger(database, "PRAGMA page_size");
  if (page_count < 0 || page_size <= 0 ||
      page_count > kMaximumSceneStoreDatabasePageImageBytes / page_size) {
    throw LogicalSceneOwnerLimitError(
        "logical-scene database page image exceeds its fixed ceiling");
  }
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

void ValidateBaseDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
          kCanonicalEmptySceneSqliteApplicationId ||
      ScalarInteger(database, "PRAGMA user_version") !=
          kCanonicalEmptySceneStoreSchemaVersion) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene SQLite base application/schema version is invalid");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  if (ReadSchemaObjects(database) != ExpectedBaseSchemaObjects()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene base schema differs from its exact v2 artifact");
  }
  ValidateMetadata(database, fixed_query, admission_digest);
  static_cast<void>(ReadDurableState(database, fixed_query));
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

void ValidateMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database,
      "SELECT schema_id,schema_version,migration_artifact_digest,"
      "maximum_scene_transactions,maximum_transaction_bytes "
      "FROM logical_scene_store_migrations ORDER BY singleton LIMIT 2");
  if (!statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
        "native projection handoff migration metadata is missing");
  }
  const auto valid =
      statement.Text(0) == kNativeProjectionHandoffStoreSchemaId &&
      statement.Integer(1) == kNativeProjectionHandoffStoreSchemaVersion &&
      statement.Text(2) == NativeHandoffMigrationArtifactDigest() &&
      statement.Integer(3) ==
          static_cast<std::int64_t>(kMaximumNativeProjectionBindings) &&
      statement.Integer(4) == kHistoricalMaximumNativeRecoveries &&
      statement.Integer(5) ==
          static_cast<std::int64_t>(kMaximumLogicalSceneTransactionBytes) &&
      !statement.Step();
  if (!valid) {
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
        "bounded logical-scene history migration metadata is missing");
  }
  const auto valid =
      statement.Text(0) == kBoundedHistoryStoreSchemaId &&
      statement.Integer(1) == kBoundedHistoryStoreSchemaVersion &&
      statement.Text(2) == BoundedHistoryMigrationArtifactDigest() &&
      statement.Integer(3) ==
          static_cast<std::int64_t>(kMaximumLogicalSceneTransactions) &&
      statement.Integer(4) == 2 &&
      !statement.Step();
  if (!valid) {
    throw LogicalSceneOwnerIntegrityError(
        "bounded logical-scene history migration metadata is invalid");
  }
}

void ValidateMaintenanceInitializationMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database,
      "SELECT schema_id,schema_version,migration_artifact_digest,"
      "maximum_initializations FROM "
      "logical_scene_maintenance_initialization_migrations "
      "ORDER BY singleton LIMIT 2");
  if (!statement.Step() ||
      statement.Text(0) != kMaintenanceInitializationSchemaId ||
      statement.Integer(1) != kMaintenanceInitializationSchemaVersion ||
      statement.Text(2) != MaintenanceInitializationMigrationArtifactDigest() ||
      statement.Integer(3) !=
          static_cast<std::int64_t>(kMaximumMaintenanceInitializations) ||
      statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "maintenance initialization migration metadata is invalid");
  }
}

void ValidateInitialSceneBaselineMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database,
      "SELECT schema_id,schema_version,migration_artifact_digest FROM "
      "logical_scene_initial_baseline_migrations WHERE singleton=1 LIMIT 2");
  if (!statement.Step() ||
      statement.Text(0) != kInitialSceneBaselineSchemaId ||
      statement.Integer(1) != kInitialSceneBaselineSchemaVersion ||
      statement.Text(2) != InitialSceneBaselineMigrationArtifactDigest() ||
      statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "initial Scene baseline migration metadata is invalid");
  }
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

void ValidateTransactionDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
          kCanonicalEmptySceneSqliteApplicationId ||
      ScalarInteger(database, "PRAGMA user_version") !=
          kLogicalSceneTransactionStoreSchemaVersion) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene SQLite transaction schema version is invalid");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  if (ReadSchemaObjects(database) != ExpectedTransactionSchemaObjects()) {
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
        "native projection handoff SQLite schema version is invalid");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  if (ReadSchemaObjects(database) != ExpectedNativeHandoffSchemaObjects()) {
    throw LogicalSceneOwnerIntegrityError(
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

void ValidateBoundedHistoryDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
          kCanonicalEmptySceneSqliteApplicationId ||
      ScalarInteger(database, "PRAGMA user_version") !=
          kBoundedHistoryStoreSchemaVersion ||
      ReadSchemaObjects(database) != ExpectedBoundedHistorySchemaObjects()) {
    throw LogicalSceneOwnerIntegrityError(
        "bounded-history logical-scene schema is invalid");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  ValidateMetadata(database, fixed_query, admission_digest);
  ValidateMigrationMetadata(database);
  ValidateNativeHandoffMigrationMetadata(database);
  ValidateBoundedHistoryMigrationMetadata(database);
}

void ValidateDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
          kCanonicalEmptySceneSqliteApplicationId ||
      ScalarInteger(database, "PRAGMA user_version") !=
          kLogicalSceneStoreSchemaVersion) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene SQLite application/schema version is invalid");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  if (ReadSchemaObjects(database) != ExpectedSchemaObjects()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene schema differs from its exact v10 artifacts");
  }
  ValidateMetadata(database, fixed_query, admission_digest);
  ValidateMigrationMetadata(database);
  ValidateNativeHandoffMigrationMetadata(database);
  ValidateBoundedHistoryMigrationMetadata(database);
  ValidateMaintenanceInitializationMigrationMetadata(database);
  ValidateInitialSceneBaselineMigrationMetadata(database);
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
             history.recovered_through_transaction_sequence != 0U) {
    throw LogicalSceneOwnerIntegrityError(
        "scene transaction history exists before bootstrap finalization");
  }
}

void ValidateMaintenanceDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
          kCanonicalEmptySceneSqliteApplicationId ||
      ScalarInteger(database, "PRAGMA user_version") !=
          kMaintenanceInitializationSchemaVersion ||
      ReadSchemaObjects(database) != ExpectedMaintenanceSchemaObjects()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene v8 schema is invalid");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  ValidateMetadata(database, fixed_query, admission_digest);
  ValidateMigrationMetadata(database);
  ValidateNativeHandoffMigrationMetadata(database);
  ValidateBoundedHistoryMigrationMetadata(database);
  ValidateMaintenanceInitializationMigrationMetadata(database);
  const auto durable = ReadDurableState(database, fixed_query);
  if (durable.finalization_receipt.has_value()) {
    static_cast<void>(ReadSceneTransactionStoreState(database, durable));
  }
}

void ValidateInitialSceneBaselineDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
          kCanonicalEmptySceneSqliteApplicationId ||
      ScalarInteger(database, "PRAGMA user_version") !=
          kInitialSceneBaselineSchemaVersion ||
      ReadSchemaObjects(database) != ExpectedSchemaObjects()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene v9 schema is invalid");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  ValidateMetadata(database, fixed_query, admission_digest);
  ValidateMigrationMetadata(database);
  ValidateNativeHandoffMigrationMetadata(database);
  ValidateBoundedHistoryMigrationMetadata(database);
  ValidateMaintenanceInitializationMigrationMetadata(database);
  ValidateInitialSceneBaselineMigrationMetadata(database);
}

// v9 provider evidence also carried the provider observation sequence, receive
// time, source timestamp and freshness budget, and every stored content hash
// and pose-update request digest covered them. Strip those fields from every
// stored document, recompute the content hash of each changed snapshot and the
// digest of each changed request, and rename every column value that names an
// old hash or digest. The immutability triggers are dropped for this rewrite
// and recreated verbatim, so the schema objects stay exactly the v9 ones.
void ConvertProviderObservationEvidence(sqlite3 *database) {
  std::vector<std::pair<std::string, std::string>> triggers;
  {
    Statement statement(
        database, "SELECT name,sql FROM sqlite_master WHERE type='trigger'");
    while (statement.Step()) {
      triggers.emplace_back(statement.Text(0), statement.Text(1));
    }
  }
  for (const auto &[name, sql] : triggers) {
    Exec(database, "DROP TRIGGER " + name);
  }

  // Rewrites `column` of every row whose document carries retired fields.
  const auto rewrite = [&](const std::string &table, const std::string &key,
                           const std::string &column, const auto &convert) {
    std::vector<std::pair<std::int64_t, std::string>> rows;
    {
      Statement select(database, "SELECT " + key + "," + column + " FROM " +
                                     table + " WHERE " + column +
                                     " IS NOT NULL");
      while (select.Step()) {
        rows.emplace_back(select.Integer(0), select.Text(1));
      }
    }
    for (const auto &[row, document] : rows) {
      const auto stripped =
          logical_scene_codec_internal::StripRetiredProviderEvidenceFields(
              document);
      if (!stripped.has_value()) {
        continue;
      }
      Statement update(database, "UPDATE " + table + " SET " + column +
                                     "=? WHERE " + key + "=?");
      update.Bind(1, convert(*stripped, row));
      update.Bind(2, row);
      if (update.Step()) {
        throw LogicalSceneOwnerIntegrityError(
            "provider evidence conversion update returned a row");
      }
    }
  };
  const auto unchanged = [](const std::string &document, std::int64_t) {
    return document;
  };

  std::map<std::string, std::string> content_hashes;
  const auto snapshot = [&](const std::string &document, std::int64_t) {
    auto value = logical_scene_codec_internal::DecodeSnapshot(document, false);
    auto content_hash =
        core::logical_scene_identity::ComputeLogicalSceneContentHash(value);
    content_hashes.insert_or_assign(value.revision.content_hash.value(),
                                    content_hash.value());
    value.revision.content_hash = std::move(content_hash);
    return logical_scene_codec_internal::EncodeSnapshot(value);
  };
  rewrite("logical_scene_transaction_intents", "transaction_sequence",
          "candidate_snapshot_json", snapshot);
  rewrite("logical_scene_history_state", "singleton",
          "checkpoint_snapshot_json", snapshot);
  rewrite("logical_scene_maintenance_initialization_intents",
          "initialization_sequence", "authoritative_snapshot_json", snapshot);
  rewrite("logical_scene_initial_baseline_intents", "initialization_sequence",
          "snapshot_json", snapshot);
  rewrite("logical_scene_transaction_intents", "transaction_sequence",
          "resolved_operation_json", unchanged);

  std::map<std::string, std::string> request_digests;
  const auto request = [&](const std::string &document,
                           const std::int64_t sequence) {
    Statement digest(database,
                     "SELECT request_digest FROM "
                     "logical_scene_transaction_intents WHERE "
                     "transaction_sequence=?");
    digest.Bind(1, sequence);
    if (!digest.Step()) {
      throw LogicalSceneOwnerIntegrityError(
          "converted scene request has no stored digest");
    }
    const auto converted = manyforge::content_identity::sha256_digest(
        logical_scene_codec_internal::EncodeRequestDigestInput(
            logical_scene_codec_internal::DecodeRequest(document)));
    request_digests.insert_or_assign(digest.Text(0), converted);
    return document;
  };
  rewrite("logical_scene_transaction_intents", "transaction_sequence",
          "request_json", request);

  const auto rename = [&](const std::map<std::string, std::string> &values,
                          const std::vector<std::pair<std::string_view,
                                                      std::string_view>>
                              &columns) {
    for (const auto &[before, after] : values) {
      for (const auto &[table, column] : columns) {
        Statement update(database, "UPDATE " + std::string(table) + " SET " +
                                       std::string(column) + "=? WHERE " +
                                       std::string(column) + "=?");
        update.Bind(1, after);
        update.Bind(2, before);
        if (update.Step()) {
          throw LogicalSceneOwnerIntegrityError(
              "provider evidence conversion rename returned a row");
        }
      }
    }
  };
  rename(content_hashes,
         {
             {"logical_scene_transaction_intents", "prior_scene_content_hash"},
             {"logical_scene_transaction_intents",
              "proposed_scene_content_hash"},
             {"logical_scene_projection_results",
              "acknowledged_scene_content_hash"},
             {"logical_scene_maintenance_initialization_intents",
              "predecessor_scene_content_hash"},
             {"logical_scene_maintenance_initialization_intents",
              "target_scene_content_hash"},
             {"logical_scene_migrated_structural_completions",
              "target_scene_content_hash"},
             {"logical_scene_history_state", "checkpoint_scene_content_hash"},
             {"logical_scene_initial_baseline_intents",
              "target_scene_content_hash"},
         });
  rename(request_digests,
         {
             {"logical_scene_transaction_intents", "request_digest"},
             {"logical_scene_request_outcomes", "request_digest"},
         });

  for (const auto &trigger : triggers) {
    Exec(database, trigger.second);
  }
  Exec(database, "PRAGMA user_version = " +
                     std::to_string(kLogicalSceneStoreSchemaVersion));
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
  statement.Bind(
      11, static_cast<std::int64_t>(kMaximumMigratedStructuralCompletions));
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
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
      4, static_cast<std::int64_t>(kMaximumNativeProjectionBindings));
  statement.Bind(5, kHistoricalMaximumNativeRecoveries);
  statement.Bind(
      6, static_cast<std::int64_t>(kMaximumLogicalSceneTransactionBytes));
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "native handoff migration metadata insert returned a row");
  }
}

void InsertBoundedHistoryMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database, "INSERT INTO logical_scene_bounded_history_migrations("
                "singleton,schema_id,schema_version,migration_artifact_digest,"
                "maximum_resident_scene_transactions,"
                "maximum_retained_native_recoveries) VALUES(1,?,?,?,?,?)");
  statement.Bind(1, kBoundedHistoryStoreSchemaId);
  statement.Bind(2, kBoundedHistoryStoreSchemaVersion);
  statement.Bind(3, BoundedHistoryMigrationArtifactDigest());
  statement.Bind(4,
                 static_cast<std::int64_t>(kMaximumLogicalSceneTransactions));
  statement.Bind(5, 2);
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "bounded-history migration metadata insert returned a row");
  }
}

void InsertMaintenanceInitializationMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database,
      "INSERT INTO logical_scene_maintenance_initialization_migrations("
      "singleton,schema_id,schema_version,migration_artifact_digest,"
      "maximum_initializations) VALUES(1,?,?,?,?)");
  statement.Bind(1, kMaintenanceInitializationSchemaId);
  statement.Bind(2, kMaintenanceInitializationSchemaVersion);
  statement.Bind(3, MaintenanceInitializationMigrationArtifactDigest());
  statement.Bind(
      4, static_cast<std::int64_t>(kMaximumMaintenanceInitializations));
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "maintenance initialization migration metadata insert returned a row");
  }
}

void InsertInitialSceneBaselineMigrationMetadata(sqlite3 *database) {
  Statement statement(
      database,
      "INSERT INTO logical_scene_initial_baseline_migrations("
      "singleton,schema_id,schema_version,migration_artifact_digest) "
      "VALUES(1,?,?,?)");
  statement.Bind(1, kInitialSceneBaselineSchemaId);
  statement.Bind(2, kInitialSceneBaselineSchemaVersion);
  statement.Bind(3, InitialSceneBaselineMigrationArtifactDigest());
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "initial Scene baseline migration metadata insert returned a row");
  }
}

void ConfigureConnection(sqlite3 *database) {
  if (sqlite3_db_config(database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) !=
          SQLITE_OK ||
      sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) !=
          SQLITE_OK) {
    throw LogicalSceneOwnerError(
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
    throw LogicalSceneOwnerError(
        "logical-scene database did not enter WAL mode");
  }
  Exec(database, "PRAGMA synchronous=FULL");
  Exec(database, "PRAGMA wal_autocheckpoint=1000");
  Exec(database, "PRAGMA journal_size_limit=16777216");
  Statement maximum_pages(database, "PRAGMA max_page_count=16384");
  if (!maximum_pages.Step() || maximum_pages.Integer(0) > kMaximumPageCount ||
      maximum_pages.Step()) {
    throw LogicalSceneOwnerLimitError(
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
    throw LogicalSceneOwnerError(
        "cannot open logical-scene database directory for fsync: " +
        std::string(std::strerror(errno)));
  }
  if (fsync(descriptor) != 0) {
    const auto detail = std::string(std::strerror(errno));
    close(descriptor);
    throw LogicalSceneOwnerError(
        "cannot fsync logical-scene database directory: " + detail);
  }
  if (close(descriptor) != 0) {
    throw LogicalSceneOwnerError(
        "cannot close logical-scene database directory: " +
        std::string(std::strerror(errno)));
  }
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

void ValidateCanonicalEmptyIntent(
    const ProjectionBootstrapIntentDTO &intent,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const RetainedBundleDTO &admitted_bundle) {
  // Check cardinality before the DTO's canonical-binding loop. A retained
  // execution-host process is infrastructure, not a native scene projection;
  // the structurally empty predecessor still has zero native bindings.
  if (!intent.native_runtime_bindings.empty()) {
    throw LogicalSceneOwnerConflictError(
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
    throw LogicalSceneOwnerConflictError(
        "bootstrap intent does not match the admitted canonical-empty "
        "cell/bundle/graph envelope");
  }
  if (graph.runtime_units.empty() !=
      bundle.compiled_products.runtime_unit_products.empty()) {
    throw LogicalSceneOwnerConflictError(
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
      throw LogicalSceneOwnerConflictError(
          "canonical-empty execution host envelope is incomplete");
    }
  }
  const auto &viewer_model = *bundle.compiled_products.viewer_model;
  if (viewer_model.product_id.empty() ||
      viewer_model.product_kind != core::RetainedProductKind::kViewerModel ||
      viewer_model.product_contract_id.empty()) {
    throw LogicalSceneOwnerConflictError(
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
      throw LogicalSceneOwnerIntegrityError(
          "qualified codec did not produce an exact canonical-empty target");
    }
    return {
        intent.bootstrap_id,
        intent.bootstrap_generation,
        std::move(target),
        std::move(snapshot),
    };
  } catch (const LogicalSceneOwnerError &) {
    throw;
  } catch (const std::exception &error) {
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerConflictError(
        "Finalize does not carry the exact generation-one canonical-empty "
        "publication receipt");
  }
  ValidateDigest(request.publication_receipt.request
                     .private_materialization_content_digest.value(),
                 "private materialization content digest");
  ValidateDigest(
      request.publication_receipt.durable_public_pointer_content_digest.value(),
      "durable public-pointer content digest");
}

void InsertFinalization(sqlite3 *database,
                        const ProjectionBootstrapFinalizeDTO &finalize) {
  const auto *transaction = std::get_if<BootstrapPublicationTransactionDTO>(
      &finalize.publication_receipt.request.transaction_ref);
  if (transaction == nullptr) {
    throw LogicalSceneOwnerConflictError(
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
  statement.Bind(14,
                 publication_request.private_materialization_content_digest
                     .value());
  statement.Bind(15, receipt.durable_public_pointer_content_digest.value());
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "finalization insert unexpectedly returned a row");
  }
}

ProjectionAggregateHealthDTO NoProjectionHealth(const AcceptedState &accepted) {
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
NoProjectionEvidence(const AcceptedState &accepted) {
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.target,
      {},
      NoProjectionHealth(accepted),
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

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

CanonicalEmptySceneAccessStatus AccessStatus(const DurableState &state) {
  const bool has_active = state.active_projection.has_value();
  const auto *maintenance = state.LatestMaintenanceInitialization();
  const bool maintenance_open =
      maintenance != nullptr && !maintenance->publication_receipt.has_value();
  const bool transition_open = maintenance_open;
  const auto *latest_maintenance = state.LatestMaintenanceInitialization();
  const bool legacy_uncertainty_repaired =
      !state.legacy_native_effects_uncertain ||
      (latest_maintenance != nullptr &&
       latest_maintenance->publication_receipt.has_value());
  const bool projection_available = legacy_uncertainty_repaired &&
                                    !state.projection_desynchronized &&
                                    !state.unresolved_scene_transaction;
  return {
      maintenance == nullptr
          ? state.phase()
          : (maintenance->publication_receipt.has_value()
                 ? CanonicalEmptySceneBootstrapPhase::kMaintenanceCompleted
                 : CanonicalEmptySceneBootstrapPhase::kMaintenanceInitializing),
      has_active && !transition_open && projection_available,
      has_active && !transition_open && projection_available,
      state.active_projection,
  };
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

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
        NoProjectionEvidence(accepted),
        state.finalization_receipt,
        NoProjectionHealth(accepted),
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
        NoProjectionEvidence(accepted),
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
      NoProjectionEvidence(accepted),
      std::nullopt,
      ErrorDTO{std::move(code), std::move(message), {}},
  };
  if (!result.presence_valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "cannot represent failed canonical-empty Finalize result");
  }
  return result;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

[[nodiscard]] const SceneObjectSnapshotDTO *
FindSceneObject(const LogicalSceneSnapshotDTO &snapshot,
                const core::SceneResourceId &scene_resource_id) {
  const auto found = std::lower_bound(
      snapshot.objects.begin(), snapshot.objects.end(), scene_resource_id,
      [](const SceneObjectSnapshotDTO &object,
         const core::SceneResourceId &id) {
        return object.scene_resource_id < id;
      });
  return found == snapshot.objects.end() ||
                 found->scene_resource_id != scene_resource_id
             ? nullptr
             : &*found;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene {

ProjectionBootstrapPrerequisiteStatusDTO
LogicalSceneOwner::ObserveProjectionBootstrapPrerequisites(
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
    throw LogicalSceneOwnerIntegrityError(
        "cannot represent command-fenced bootstrap prerequisites");
  }
  return ready;
}

ProjectionBootstrapAcceptedDTO
LogicalSceneOwner::BeginProjectionBootstrap(
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
      throw LogicalSceneOwnerConflictError(
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
    throw LogicalSceneOwnerConflictError(
        "Begin atomic prerequisite recheck is not command-fenced empty");
  }
  const auto accepted = MintAcceptedState(intent, impl_->fixed_query_);
  InsertAcceptance(database, accepted);
  state = ReadCompleteDurableState(database, impl_->fixed_query_);
  if (!state.accepted.has_value() || state.rebuilt ||
      state.finalization_receipt.has_value() ||
      state.accepted->target != accepted.target) {
    throw LogicalSceneOwnerIntegrityError(
        "durable Begin acceptance did not replay exactly");
  }
  transaction.Commit();
  return AcceptedResult(*state.accepted);
}

ProjectionBootstrapRebuildResultDTO
LogicalSceneOwner::RebuildProjectionBootstrap(
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
    throw LogicalSceneOwnerConflictError(
        "Rebuild does not match the exact durable Begin acceptance");
  }
  if (!state.rebuilt) {
    InsertRebuild(database, *state.accepted);
    state = ReadCompleteDurableState(database, impl_->fixed_query_);
  }
  if (!state.rebuilt) {
    throw LogicalSceneOwnerIntegrityError(
        "zero-native Rebuild was not durably recorded");
  }
  const auto evidence = NoProjectionEvidence(*state.accepted);
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
    throw LogicalSceneOwnerIntegrityError(
        "cannot represent exact healthy zero-native Rebuild evidence");
  }
  transaction.Commit();
  return result;
}

ProjectionBootstrapStatusDTO
LogicalSceneOwner::ProjectionBootstrapStatus(
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
    throw LogicalSceneOwnerIntegrityError(
        "durable state cannot form a valid bootstrap status");
  }
  return result;
}

ProjectionBootstrapFinalizeResultDTO
LogicalSceneOwner::FinalizeProjectionBootstrap(
    const ProjectionBootstrapFinalizeDTO &request) {
  if (!request.valid()) {
    throw std::invalid_argument("bootstrap Finalize request is invalid");
  }
  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto state = impl_->ReadStateTransaction();
  if (!state.accepted.has_value() || !state.rebuilt) {
    throw LogicalSceneOwnerConflictError(
        "Finalize requires the exact durable zero-native Rebuild evidence");
  }
  ValidateFinalizeRequest(request, *state.accepted);
  if (state.finalization_receipt.has_value()) {
    if (*state.finalization_receipt != request.publication_receipt) {
      throw LogicalSceneOwnerConflictError(
          "Finalize replay carries a different publication receipt");
    }
    ProjectionBootstrapFinalizeResultDTO replay{
        state.accepted->bootstrap_id,
        state.accepted->bootstrap_generation,
        state.accepted->target,
        *state.finalization_receipt,
        ProjectionBootstrapFinalizeResultState::kFinalized,
        NoProjectionEvidence(*state.accepted),
        NoProjectionHealth(*state.accepted),
        std::nullopt,
    };
    if (!replay.presence_valid()) {
      throw LogicalSceneOwnerIntegrityError(
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
    throw LogicalSceneOwnerIntegrityError(
        "durable bootstrap state changed before Finalize commit");
  }
  ValidateFinalizeRequest(request, *state.accepted);
  if (!state.finalization_receipt.has_value()) {
    InsertFinalization(database, request);
    state = ReadCompleteDurableState(database, impl_->fixed_query_);
  }
  if (!state.finalization_receipt.has_value() ||
      *state.finalization_receipt != request.publication_receipt) {
    throw LogicalSceneOwnerIntegrityError(
        "Finalize receipt was not durably recorded exactly");
  }
  transaction.Commit();

  ProjectionBootstrapFinalizeResultDTO result{
      state.accepted->bootstrap_id,
      state.accepted->bootstrap_generation,
      state.accepted->target,
      *state.finalization_receipt,
      ProjectionBootstrapFinalizeResultState::kFinalized,
      NoProjectionEvidence(*state.accepted),
      NoProjectionHealth(*state.accepted),
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "durable Finalize state cannot form a valid result");
  }
  return result;
}

std::optional<SceneCommitResultDTO> LogicalSceneOwner::ReadCommitResult(
    const SceneTransactionRequestDTO &request) const {
  ValidateBoundedIdentity(request.request_context.request_id.value(),
                          "request_id");
  ValidateBoundedIdentity(request.request_context.request_owner_epoch.value(),
                          "request_owner_epoch");
  const ContentDigest digest(manyforge::content_identity::sha256_digest(
      logical_scene_codec_internal::EncodeRequestDigestInput(request)));
  std::lock_guard lock(impl_->mutex_);
  const auto result = ReadOutcomeForRequest(
      impl_->database(), request.request_context.request_owner_epoch.value(),
      request.request_context.request_id);
  if (!result.has_value())
    return std::nullopt;
  if (result->request_digest != digest)
    throw LogicalSceneOwnerConflictError(
        "scene result lookup changed the exact request");
  return ResultFromStoredOutcome(impl_->database(), *result,
                                 core::RequestReplayStatusDTO::kReplayedExact);
}

CanonicalEmptySceneStoreIntegrityReport
LogicalSceneOwner::VerifyIntegrity() const {
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
      ScalarInteger(
          database,
          "SELECT count(*) FROM logical_scene_migrated_structural_completions"),
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
          state.legacy_native_effects_uncertain,
      AccessStatus(state),
  };
  transaction.Commit();
  if (!report.access.valid() || report.acceptance_row_count < 0 ||
      report.acceptance_row_count > 1 || report.rebuild_row_count < 0 ||
      report.rebuild_row_count > 1 || report.finalization_row_count < 0 ||
      report.finalization_row_count > 1 ||
      report.migrated_structural_completion_count < 0 ||
      report.migrated_structural_completion_count >
          static_cast<std::int64_t>(kMaximumMigratedStructuralCompletions) ||
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
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene integrity report violates fixed row bounds");
  }
  return report;
}

} // namespace manyforge::scene
