#include "logical_scene_owner_impl.hpp"

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

constexpr std::int64_t kPageSizeBytes = 4096;

constexpr std::int64_t kMaximumPageCount =
    kMaximumSceneStoreDatabasePageImageBytes / kPageSizeBytes;

constexpr std::int64_t kExpectedSchemaObjectCardinality = 63;

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

std::int64_t ScalarInteger(sqlite3 *database, const std::string_view sql,
                           PersistentStatementCache *cache) {
  Statement statement(database, sql, cache);
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

/// The current schema: the numbered resources applied in order, as for a new
/// store.
SchemaObjects BuildExpectedSchemaObjects() {
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
    Exec(database, detail::kEmbeddedLogicalSceneTransactionMigration);
    Exec(database, detail::kEmbeddedNativeProjectionHandoffMigration);
    Exec(database, detail::kEmbeddedBoundedHistoryMigration);
    Exec(database, detail::kEmbeddedMaintenanceInitializationMigration);
    Exec(database, detail::kEmbeddedInitialSceneBaselineMigration);
    auto objects = ReadSchemaObjects(database);
    if (objects.size() !=
        static_cast<std::size_t>(kExpectedSchemaObjectCardinality)) {
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

const SchemaObjects &ExpectedSchemaObjects() {
  static const SchemaObjects expected = BuildExpectedSchemaObjects();
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

/// Returns the exact Scene-owned current native binding set: the one of the
/// latest published maintenance completion, else none.
[[nodiscard]] std::vector<core::ProjectionExecutionHostBindingDTO>
ActiveExecutionHostBindings(const DurableState &state) {
  for (auto initialization = state.maintenance_initializations.rbegin();
       initialization != state.maintenance_initializations.rend();
       ++initialization) {
    if (initialization->publication_receipt.has_value()) {
      return initialization->bindings;
    }
  }
  return {};
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

struct StoredSceneIntent final {
  std::uint64_t transaction_sequence;
  RequestKey request_key;
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
  Statement statement(
      database, "SELECT compaction_generation,"
                "compacted_through_transaction_sequence,"
                "compacted_through_owner_event_sequence,"
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
  state.compacted_through_transaction_sequence =
      nonnegative(1, "transaction watermark");
  state.compacted_through_owner_event_sequence =
      nonnegative(2, "owner-event watermark");
  const auto maintenance_count =
      nonnegative(3, "maintenance completion count");
  if (maintenance_count > kMaximumMaintenanceInitializations) {
    throw LogicalSceneOwnerIntegrityError(
        "bounded logical-scene maintenance completion count exceeds its bound");
  }
  state.compacted_maintenance_completion_count =
      static_cast<std::size_t>(maintenance_count);
  state.recovered_through_transaction_sequence =
      nonnegative(4, "recovered transaction watermark");
  const bool checkpoint_present = !statement.IsNull(5);
  for (int column = 6; column <= 10; ++column) {
    if (statement.IsNull(column) == checkpoint_present) {
      throw LogicalSceneOwnerIntegrityError(
          "bounded logical-scene checkpoint has partial presence");
    }
  }
  if (checkpoint_present) {
    const auto revision_id = statement.Integer(8);
    if (revision_id <= 0) {
      throw LogicalSceneOwnerIntegrityError(
          "bounded logical-scene checkpoint revision is invalid");
    }
    ProjectionRevisionDTO projection{
        RetainedBundleId(statement.Text(5)), GraphRevision(statement.Text(6)),
        SceneRevision{SceneId(statement.Text(7)),
                      static_cast<std::uint64_t>(revision_id),
                      ContentDigest(statement.Text(9))}};
    auto snapshot =
        logical_scene_codec_internal::DecodeSnapshot(statement.Text(10));
    if (!projection.valid() || snapshot.revision != projection.scene_revision) {
      throw LogicalSceneOwnerIntegrityError(
          "bounded logical-scene checkpoint content is inconsistent");
    }
    state.checkpoint_projection = std::move(projection);
    state.checkpoint_snapshot = std::move(snapshot);
  }
  if (statement.Integer(11) != 0 || statement.Step() ||
      (state.compaction_generation == 0U) !=
          !state.checkpoint_projection.has_value() ||
      state.checkpoint_projection.has_value() !=
          state.checkpoint_snapshot.has_value()) {
    throw LogicalSceneOwnerIntegrityError(
        "bounded logical-scene history state violates its closed shape");
  }
  if (state.compaction_generation >= kMaximumSafeIntegral) {
    throw LogicalSceneOwnerIntegrityError(
        "compacted logical-scene history generation is exhausted");
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
ReadOutcomeForRequest(sqlite3 *database, const RequestKey &request_key) {
  Statement statement(database,
                      "SELECT request_key,request_digest,"
                      "transaction_sequence,success,projection_status,"
                      "attempted_projection_count,error_code,error_message,"
                      "error_details_json FROM logical_scene_request_outcomes "
                      "WHERE request_key=? LIMIT 2");
  statement.Bind(1, request_key);
  if (!statement.Step()) {
    return std::nullopt;
  }
  StoredSceneOutcome outcome{
      statement.Key(0),
      ContentDigest(statement.Text(1)),
      statement.IsNull(2)
          ? std::nullopt
          : std::optional<std::uint64_t>(
                static_cast<std::uint64_t>(statement.Integer(2))),
      statement.Integer(3) == 1,
      DecodeProjectionStatus(statement.Text(4)),
      static_cast<std::size_t>(statement.Integer(5)),
      std::nullopt,
  };
  if (outcome.transaction_sequence.has_value() &&
      *outcome.transaction_sequence == 0U) {
    throw LogicalSceneOwnerIntegrityError(
        "stored scene outcome has a zero transaction sequence");
  }
  if (outcome.success) {
    if (!statement.IsNull(6) || !statement.IsNull(7) || !statement.IsNull(8)) {
      throw LogicalSceneOwnerIntegrityError(
          "successful scene outcome carries an error");
    }
  } else {
    if (statement.IsNull(6) || statement.IsNull(7) || statement.IsNull(8)) {
      throw LogicalSceneOwnerIntegrityError(
          "failed scene outcome lacks its error");
    }
    outcome.error = ErrorDTO{
        statement.Text(6),
        statement.Text(7),
        logical_scene_codec_internal::DecodeErrorDetails(statement.Text(8)),
    };
  }
  if (statement.Step() ||
      !core::IsExactLowercaseSha256Digest(outcome.request_digest.value()) ||
      outcome.request_key != request_key ||
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

std::optional<StoredSceneOutcome>
ReadOutcomeForRequest(const SceneStoreMemory &memory,
                      const RequestKey &request_key) {
  const auto found = memory.outcomes.find(request_key);
  if (found == memory.outcomes.end()) {
    return std::nullopt;
  }
  return found->second;
}

void BindRequestKey(persistence::SqliteRecord &record, const int index,
                    const RequestKey &key) {
  record.BindBlob(index,
                  std::string_view(
                      reinterpret_cast<const char *>(key.bytes.data()),
                      key.bytes.size()));
}

void BindRequestKey(SizedRow &row, const int index, const RequestKey &key) {
  row.BindBlob(index, std::string_view(
                          reinterpret_cast<const char *>(key.bytes.data()),
                          key.bytes.size()));
}

std::uint64_t SceneFileBytes(const SceneStoreMemory &memory) {
  auto bytes = memory.transaction_file_bytes;
  for (const auto &[sequence, rows] : memory.maintenance_file_bytes) {
    bytes += rows;
  }
  return bytes;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

std::optional<StoredSceneOutcome>
ReadOutcomeForSequence(sqlite3 *database,
                       const std::uint64_t transaction_sequence) {
  Statement key(database, "SELECT request_key FROM "
                          "logical_scene_transaction_intents WHERE "
                          "transaction_sequence=? LIMIT 2");
  key.Bind(1, CheckedSafeCounter(transaction_sequence, "transaction_sequence"));
  if (!key.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "scene outcome lookup has no matching intent");
  }
  const auto request_key = key.Key(0);
  if (key.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "scene intent sequence is not unique");
  }
  return ReadOutcomeForRequest(database, request_key);
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
  std::optional<core::ProjectionExecutionHostBindingDTO> previous_binding;
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
    core::ProjectionExecutionHostBindingDTO binding{
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

// The same summary from the memory rows; the per-row shape checks ran when
// each result was recorded or, for a reopened store, at open.
ProjectionEvidenceState
ReadProjectionEvidenceState(const SceneTransactionRecord &record) {
  ProjectionEvidenceState evidence;
  evidence.attempt_count = record.projection_attempt_count;
  for (const auto state : record.projection_results) {
    switch (state) {
    case core::ProjectionCommitResultStateDTO::kCommitted:
      evidence.any_result_committed = true;
      evidence.all_results_rollback_proven = false;
      break;
    case core::ProjectionCommitResultStateDTO::kRejectedRollbackProven:
      evidence.all_results_committed = false;
      break;
    case core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain:
      evidence.all_results_committed = false;
      evidence.all_results_rollback_proven = false;
      evidence.any_result_uncertain = true;
      break;
    case core::ProjectionCommitResultStateDTO::kAccepted:
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
  if (history.compacted_through_transaction_sequence >= kMaximumSafeIntegral ||
      history.compacted_through_owner_event_sequence >= kMaximumSafeIntegral) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene history watermarks cannot admit another record");
  }
  SceneTransactionStoreState state{
      replay_projection,
      replay_snapshot,
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
  // `committed_sequence` names a Scene transaction that committed (0 when the
  // next event is not a committed transaction; sequences start at 1). Every
  // completion captured before it (lower snapshot sequence) precedes it, even
  // when an Apply chain returned to its prior composite (A -> B -> A, such as
  // Close -> Load of the same Scenario, which preserves the Scene revision).
  const auto apply_maintenance_until =
      [&](const std::optional<ProjectionRevisionDTO> &expected,
          const std::uint64_t committed_sequence) {
        while (next_maintenance_completion < completed_maintenance.size()) {
          const auto &initialization =
              *completed_maintenance[next_maintenance_completion];
          const bool captured_before_commit =
              committed_sequence != 0U &&
              initialization.authoritative_snapshot.transaction_sequence <
                  committed_sequence;
          // A configuration-only Apply may leave the composite unchanged.
          // Consume those completions before the next Scene transaction, but
          // not across a no-op transaction that kept the same revision.
          if (expected.has_value() && state.active_projection == *expected &&
              !captured_before_commit &&
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
      "SELECT transaction_sequence,request_key,"
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
        statement.Key(1),
        ContentDigest(statement.Text(2)),
        statement.Text(3),
        ReadProjectionColumns(statement, 4),
        ReadProjectionColumns(statement, 9),
        statement.Text(14),
        statement.IsNull(15)
            ? std::nullopt
            : std::optional<ContentDigest>(ContentDigest(statement.Text(15))),
        logical_scene_codec_internal::DecodeSnapshot(statement.Text(16)),
        DecodeUpdateStatus(statement.Text(17)),
        static_cast<std::size_t>(statement.Integer(18)),
    };
    const auto stored_request =
        logical_scene_codec_internal::DecodeRequest(intent.request_json);
    if (!core::IsExactLowercaseSha256Digest(intent.request_digest.value()) ||
        intent.request_json.empty() ||
        intent.request_json.size() > kMaximumLogicalSceneTransactionBytes ||
        manyforge::content_identity::sha256_digest(
            logical_scene_codec_internal::EncodeRequestDigestInput(
                stored_request)) != intent.request_digest.value() ||
        stored_request.request_context.request_key != intent.request_key ||
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
  Statement events(
      database,
      "SELECT owner_event_sequence,event_kind,transaction_sequence "
      "FROM logical_scene_owner_events "
      "WHERE owner_event_sequence>? ORDER BY owner_event_sequence LIMIT 257");
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
      if (events.IsNull(2)) {
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
      const std::uint64_t committed_sequence =
          record.outcome.has_value() && record.outcome->success
              ? record.intent.transaction_sequence
              : 0U;
      if (transaction_event_seen[index] ||
          !apply_maintenance_until(record.intent.prior, committed_sequence)) {
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
  static_cast<void>(apply_maintenance_until(std::nullopt, 0U));
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

// The same replay over the memory rows. Checks of stored bytes (types,
// encodings, digests of the stored JSON) ran at open; the replay checks stay.
SceneTransactionStoreState
ReadSceneTransactionStoreState(const SceneStoreMemory &memory,
                               const DurableState &durable_state) {
  if (!durable_state.accepted.has_value() ||
      !durable_state.finalization_receipt.has_value()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene transaction history requires finalized bootstrap");
  }
  const auto &bootstrap_projection = durable_state.accepted->target;
  const auto &bootstrap_snapshot = durable_state.accepted->snapshot;
  const auto &history = memory.history;
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
  if (history.compacted_through_transaction_sequence >= kMaximumSafeIntegral ||
      history.compacted_through_owner_event_sequence >= kMaximumSafeIntegral) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene history watermarks cannot admit another record");
  }
  SceneTransactionStoreState state{
      replay_projection,
      replay_snapshot,
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
  // See the file replay above: completions captured before a committed
  // transaction precede it, and a no-op revision is not crossed.
  const auto apply_maintenance_until =
      [&](const std::optional<ProjectionRevisionDTO> &expected,
          const std::uint64_t committed_sequence) {
        while (next_maintenance_completion < completed_maintenance.size()) {
          const auto &initialization =
              *completed_maintenance[next_maintenance_completion];
          const bool captured_before_commit =
              committed_sequence != 0U &&
              initialization.authoritative_snapshot.transaction_sequence <
                  committed_sequence;
          if (expected.has_value() && state.active_projection == *expected &&
              !captured_before_commit &&
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
  struct ReplayedRecord final {
    const SceneTransactionRecord *intent;
    std::optional<StoredSceneOutcome> outcome;
  };
  std::vector<ReplayedRecord> records;
  std::uint64_t expected_sequence = state.next_transaction_sequence;
  for (const auto &intent : memory.transactions) {
    if (intent.transaction_sequence <=
        history.compacted_through_transaction_sequence) {
      continue;
    }
    if (!records.empty() &&
        (!records.back().outcome.has_value() ||
         (records.back().outcome->projection_status ==
              SceneProjectionCommitStatusDTO::kFailedProjectionUncertain &&
          records.back().intent->transaction_sequence >
              history.recovered_through_transaction_sequence))) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene history continues after an unresolved or "
          "desynchronized transaction");
    }
    if (intent.transaction_sequence != expected_sequence) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene transaction sequence is not contiguous");
    }
    if (intent.required_projection_count > 256U ||
        intent.proposed.retained_bundle_id != intent.prior.retained_bundle_id ||
        intent.proposed.graph_revision != intent.prior.graph_revision ||
        intent.proposed.scene_revision.scene_id !=
            intent.prior.scene_revision.scene_id ||
        intent.prior.scene_revision.scene_id !=
            bootstrap_snapshot.revision.scene_id) {
      throw LogicalSceneOwnerIntegrityError(
          "durable logical-scene transaction intent is invalid");
    }
    const auto projection_evidence = ReadProjectionEvidenceState(intent);
    const auto attempts = projection_evidence.attempt_count;
    const auto results = projection_evidence.result_count;
    if (attempts > intent.required_projection_count || results > attempts) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene projection evidence cardinality is invalid");
    }
    const auto outcome = ReadOutcomeForRequest(memory, intent.request_key);
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
    records.push_back(ReplayedRecord{&intent, outcome});
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
  std::uint64_t expected_event_sequence = state.next_owner_event_sequence;
  std::size_t resident_event_count = 0U;
  bool history_sealed = false;
  for (const auto &event : memory.owner_events) {
    if (event.owner_event_sequence <=
        history.compacted_through_owner_event_sequence) {
      continue;
    }
    if (history_sealed) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene owner history continues after an unresolved or "
          "desynchronized event");
    }
    if (++resident_event_count > kMaximumLogicalSceneOwnerEvents ||
        expected_event_sequence > kMaximumSafeIntegral ||
        event.owner_event_sequence != expected_event_sequence) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene owner event sequence is invalid");
    }
    const auto sequence = event.transaction_sequence;
    if (sequence == 0U ||
        sequence <= history.compacted_through_transaction_sequence ||
        sequence >= expected_sequence) {
      throw LogicalSceneOwnerIntegrityError(
          "scene transaction owner event references an absent intent");
    }
    const auto index = static_cast<std::size_t>(
        sequence - history.compacted_through_transaction_sequence - 1U);
    if (index >= records.size()) {
      throw LogicalSceneOwnerIntegrityError(
          "scene transaction owner event exceeds resident history");
    }
    auto &record = records[index];
    const std::uint64_t committed_sequence =
        record.outcome.has_value() && record.outcome->success
            ? record.intent->transaction_sequence
            : 0U;
    if (transaction_event_seen[index] ||
        !apply_maintenance_until(record.intent->prior, committed_sequence)) {
      throw LogicalSceneOwnerIntegrityError(
          "scene transaction event does not follow the active composite");
    }
    transaction_event_seen[index] = true;
    if (!record.outcome.has_value()) {
      history_sealed = true;
    } else if (record.outcome->success) {
      state.active_projection = record.intent->proposed;
      state.active_snapshot = logical_scene_codec_internal::DecodeSnapshot(
          record.intent->candidate_snapshot_json);
    } else if (record.outcome->projection_status ==
                   SceneProjectionCommitStatusDTO::kFailedProjectionUncertain &&
               record.intent->transaction_sequence >
                   history.recovered_through_transaction_sequence) {
      history_sealed = true;
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
  static_cast<void>(apply_maintenance_until(std::nullopt, 0U));
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

void InsertSceneOutcome(SceneStoreMemory &memory,
                        persistence::SqliteRecord &record,
                        const RequestKey &request_key,
                        const ContentDigest &request_digest,
                        const std::optional<std::uint64_t> transaction_sequence,
                        const bool success,
                        const SceneProjectionCommitStatusDTO projection_status,
                        const std::size_t attempted_projection_count,
                        const std::optional<ErrorDTO> &error) {
  SizedRow row(
      record,
      "INSERT INTO logical_scene_request_outcomes("
      "request_key,request_digest,transaction_sequence,"
      "success,projection_status,attempted_projection_count,error_code,"
      "error_message,error_details_json) VALUES(?,?,?,?,?,?,?,?,?)");
  BindRequestKey(row, 1, request_key);
  row.Bind(2, request_digest.value());
  if (transaction_sequence.has_value()) {
    row.Bind(
        3, CheckedSafeCounter(*transaction_sequence, "transaction_sequence"));
  } else {
    row.BindNull(3);
  }
  row.Bind(4, std::int64_t{success ? 1 : 0});
  row.Bind(5, core::to_string(projection_status));
  row.Bind(6, static_cast<std::int64_t>(attempted_projection_count));
  // Memory holds the error as the file returns it: details decoded from
  // their stored encoding.
  std::optional<ErrorDTO> stored_error;
  if (error.has_value()) {
    auto details_json =
        logical_scene_codec_internal::EncodeErrorDetails(error->details);
    stored_error = ErrorDTO{
        error->code, error->message,
        logical_scene_codec_internal::DecodeErrorDetails(details_json)};
    row.Bind(7, error->code);
    row.Bind(8, error->message);
    row.Bind(9, details_json);
  } else {
    row.BindNull(7);
    row.BindNull(8);
    row.BindNull(9);
  }
  memory.transaction_file_bytes += SceneRowFileBytes(
      row.record_bytes(), SceneTable("logical_scene_request_outcomes"));
  memory.outcomes.emplace(
      request_key,
      StoredSceneOutcome{request_key, request_digest, transaction_sequence,
                         success, projection_status,
                         attempted_projection_count, std::move(stored_error)});
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

SceneCommitResultDTO
ResultFromStoredOutcome(const SceneStoreMemory &memory,
                        const StoredSceneOutcome &outcome,
                        const core::RequestReplayStatusDTO replay_status) {
  if (!outcome.success) {
    return FailedSceneResult(outcome.request_key, replay_status, *outcome.error,
                             outcome.projection_status);
  }
  const auto sequence = *outcome.transaction_sequence;
  const auto intent = std::find_if(
      memory.transactions.begin(), memory.transactions.end(),
      [&](const SceneTransactionRecord &record) {
        return record.transaction_sequence == sequence;
      });
  if (intent == memory.transactions.end()) {
    throw LogicalSceneOwnerIntegrityError(
        "scene result references an absent transaction intent");
  }
  SceneCommitResultDTO result{
      outcome.request_key,
      replay_status,
      true,
      std::nullopt,
      sequence,
      intent->proposed,
      SceneProjectionCommitStatusDTO::kCommitted,
      intent->update_status,
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
      "object_count,carried_attachment_count,execution_host_binding_count "
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

// The columns of the finalization row.
struct FinalizationColumns final {
  std::string bootstrap_id;
  std::int64_t bootstrap_generation;
  std::int64_t publication_generation;
  std::string publication_bootstrap_id;
  std::int64_t publication_bootstrap_generation;
  std::string candidate_retained_bundle_id;
  std::string candidate_graph_revision;
  std::string candidate_scene_id;
  std::int64_t candidate_scene_revision_id;
  std::string candidate_scene_content_hash;
  std::string request_retained_bundle_id;
  std::string request_graph_revision;
  std::string private_materialization_digest;
  std::string compatibility_materialization_digest;
  std::string public_pointer_digest;
};

// The receipt the finalization row stands for, checked against its
// acceptance.
ActiveBundlePublicationReceiptDTO
FinalizationReceipt(const FinalizationColumns &row,
                    const std::optional<AcceptedState> &accepted,
                    const bool rebuilt) {
  const auto &bootstrap_id = row.bootstrap_id;
  const auto bootstrap_generation = row.bootstrap_generation;
  const auto publication_generation = row.publication_generation;
  const auto &publication_bootstrap_id = row.publication_bootstrap_id;
  const auto publication_bootstrap_generation =
      row.publication_bootstrap_generation;
  const auto &candidate_retained_bundle_id = row.candidate_retained_bundle_id;
  const auto &candidate_graph_revision = row.candidate_graph_revision;
  const auto &candidate_scene_id = row.candidate_scene_id;
  const auto candidate_scene_revision_id = row.candidate_scene_revision_id;
  const auto &candidate_scene_content_hash = row.candidate_scene_content_hash;
  const auto &request_retained_bundle_id = row.request_retained_bundle_id;
  const auto &request_graph_revision = row.request_graph_revision;
  const auto &private_materialization_digest =
      row.private_materialization_digest;
  const auto &compatibility_materialization_digest =
      row.compatibility_materialization_digest;
  const auto &public_pointer_digest = row.public_pointer_digest;
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
  const FinalizationColumns row{
      statement.Text(0),  statement.Integer(1), statement.Integer(2),
      statement.Text(3),  statement.Integer(4), statement.Text(5),
      statement.Text(6),  statement.Text(7),    statement.Integer(8),
      statement.Text(9),  statement.Text(10),   statement.Text(11),
      statement.Text(12), statement.Text(13),   statement.Text(14),
  };
  if (statement.Step()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene finalization cardinality exceeds one");
  }
  return FinalizationReceipt(row, accepted, rebuilt);
}

LogicalSceneSnapshotDTO ReadMaintenanceSnapshot(
    MaintenanceSnapshotCache *cache, const std::int64_t sequence,
    const bool baseline, std::string bytes) {
  if (cache != nullptr) {
    if (const auto *entry = cache->Find(sequence, baseline, bytes)) {
      return entry->snapshot;
    }
  }
  auto snapshot = logical_scene_codec_internal::DecodeSnapshot(bytes);
  if (cache != nullptr) {
    cache->Store(sequence, baseline, std::move(bytes), snapshot);
  }
  return snapshot;
}

ContentDigest ReadMaintenanceSnapshotHash(
    MaintenanceSnapshotCache *cache, const std::int64_t sequence,
    const std::string_view bytes, const LogicalSceneSnapshotDTO &snapshot) {
  auto *entry = cache == nullptr ? nullptr : cache->Find(sequence, true, bytes);
  if (entry != nullptr && entry->content_hash.has_value()) {
    return *entry->content_hash;
  }
  auto hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot);
  if (entry != nullptr) {
    entry->content_hash = hash;
  }
  return hash;
}

std::vector<MaintenanceInitializationState>
ReadMaintenanceInitializations(sqlite3 *database,
                               const CellId &expected_cell_id,
                               MaintenanceSnapshotCache *cache,
                               PersistentStatementCache *statements) {
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
      "ORDER BY initialization_sequence LIMIT 257", statements);
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
        (!result.empty() &&
         static_cast<std::uint64_t>(sequence) <=
             result.back().initialization_sequence)) {
      throw LogicalSceneOwnerIntegrityError(
          "maintenance initialization intent is structurally invalid");
    }
    MaintenanceInitializationState state{
        static_cast<std::uint64_t>(sequence),
        ApplyId(static_cast<std::uint64_t>(intents.Integer(1))),
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
        ReadMaintenanceSnapshot(cache, sequence, false, intents.Text(13)),
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
        false,
    };
    if (!state.apply_id.nonzero() || !state.apply_generation.nonzero() ||
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
        "initialization_sequence=? ORDER BY binding_ordinal LIMIT 257", statements);
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

    Statement baseline_request(
        database,
        "SELECT request_digest FROM logical_scene_initial_baseline_requests "
        "WHERE initialization_sequence=? LIMIT 2", statements);
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
          "initialization_sequence=? LIMIT 2", statements);
      baseline_intent.Bind(1, sequence);
      std::optional<ProjectionRevisionDTO> final_target;
      std::optional<LogicalSceneSnapshotDTO> final_snapshot;
      if (baseline_intent.Step()) {
        state.initial_baseline_intent_recorded = true;
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
        const auto baseline_bytes = baseline_intent.Text(5);
        final_snapshot = ReadMaintenanceSnapshot(cache, sequence, true,
                                                 baseline_bytes);
        if (!final_target->valid() ||
            final_target->retained_bundle_id != state.target.retained_bundle_id ||
            final_target->graph_revision != state.target.graph_revision ||
            final_target->scene_revision.scene_id !=
                state.target.scene_revision.scene_id ||
            final_target->scene_revision.revision_id <=
                state.target.scene_revision.revision_id ||
            final_snapshot->revision != final_target->scene_revision ||
            final_snapshot->cell_id != expected_cell_id ||
            ReadMaintenanceSnapshotHash(cache, sequence, baseline_bytes,
                                        *final_snapshot) !=
                final_target->scene_revision.content_hash ||
            baseline_intent.Step()) {
          throw LogicalSceneOwnerIntegrityError(
              "initial Scene baseline target is not an exact successor");
        }
      }
      Statement baseline_result(
          database,
          "SELECT state,first_error_code,first_error_message FROM "
          "logical_scene_initial_baseline_results WHERE "
          "initialization_sequence=? LIMIT 2", statements);
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

    Statement outcome(
        database,
        "SELECT state,first_error_code,first_error_message,effects_unknown "
        "FROM logical_scene_maintenance_initialization_results WHERE "
        "initialization_sequence=? LIMIT 2", statements);
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
        "initialization_sequence=? LIMIT 2", statements);
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
namespace {

DurableState
ReadDurableState(sqlite3 *database,
                 const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
                 MaintenanceSnapshotCache *cache = nullptr,
                 PersistentStatementCache *statements = nullptr) {
  DurableState state;
  state.accepted = ReadAcceptance(database, fixed_query);
  state.rebuilt = ReadRebuild(database, state.accepted);
  state.finalization_receipt =
      ReadFinalization(database, state.accepted, state.rebuilt);
  state.maintenance_initializations = ReadMaintenanceInitializations(
      database, fixed_query.cell_id, cache, statements);
  if (state.accepted.has_value() && state.finalization_receipt.has_value()) {
    state.active_projection = state.accepted->target;
    state.active_scene_snapshot = state.accepted->snapshot;
    for (const auto &initialization : state.maintenance_initializations) {
      if (initialization.publication_receipt.has_value()) {
        state.active_projection = initialization.target;
      }
    }
  }
  return state;
}

DurableState ReadDurableState(const SceneStoreMemory &memory) {
  DurableState state;
  state.accepted = memory.accepted;
  state.rebuilt = memory.rebuilt;
  state.finalization_receipt = memory.finalization_receipt;
  state.maintenance_initializations = memory.maintenance_initializations;
  if (state.accepted.has_value() && state.finalization_receipt.has_value()) {
    state.active_projection = state.accepted->target;
    state.active_scene_snapshot = state.accepted->snapshot;
    for (const auto &initialization : state.maintenance_initializations) {
      if (initialization.publication_receipt.has_value()) {
        state.active_projection = initialization.target;
      }
    }
  }
  return state;
}

core::ProjectionCommitResultStateDTO
DecodeProjectionResultState(const std::string_view value) {
  if (value == "committed") {
    return core::ProjectionCommitResultStateDTO::kCommitted;
  }
  if (value == "rejected_rollback_proven") {
    return core::ProjectionCommitResultStateDTO::kRejectedRollbackProven;
  }
  if (value == "failed_projection_uncertain") {
    return core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain;
  }
  throw LogicalSceneOwnerIntegrityError(
      "stored projection result state is invalid");
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

SceneStoreMemory LoadSceneStoreMemory(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query) {
  SceneStoreMemory memory;
  auto durable = ReadDurableState(database, fixed_query);
  memory.accepted = std::move(durable.accepted);
  memory.rebuilt = durable.rebuilt;
  memory.finalization_receipt = std::move(durable.finalization_receipt);
  memory.maintenance_initializations =
      std::move(durable.maintenance_initializations);
  memory.history = ReadBoundedHistoryState(database);

  Statement intents(
      database,
      "SELECT transaction_sequence,request_key,request_digest,request_json,"
      "prior_retained_bundle_id,prior_graph_revision,prior_scene_id,"
      "prior_scene_revision_id,prior_scene_content_hash,"
      "proposed_retained_bundle_id,proposed_graph_revision,"
      "proposed_scene_id,proposed_scene_revision_id,"
      "proposed_scene_content_hash,candidate_snapshot_json,update_status,"
      "required_projection_count FROM logical_scene_transaction_intents "
      "ORDER BY transaction_sequence");
  while (intents.Step()) {
    const auto sequence = intents.Integer(0);
    Statement attempts(database,
                       "SELECT count(*) FROM logical_scene_projection_attempts "
                       "WHERE transaction_sequence=?");
    attempts.Bind(1, sequence);
    if (!attempts.Step()) {
      throw LogicalSceneOwnerIntegrityError("SQLite scalar row is missing");
    }
    const auto attempt_count = attempts.Integer(0);
    std::vector<core::ProjectionCommitResultStateDTO> results;
    Statement result_rows(
        database,
        "SELECT result_state FROM logical_scene_projection_results WHERE "
        "transaction_sequence=? ORDER BY projection_ordinal");
    result_rows.Bind(1, sequence);
    while (result_rows.Step()) {
      results.push_back(DecodeProjectionResultState(result_rows.Text(0)));
    }
    memory.transactions.push_back(SceneTransactionRecord{
        static_cast<std::uint64_t>(sequence),
        intents.Key(1),
        ContentDigest(intents.Text(2)),
        intents.Text(3),
        ReadProjectionColumns(intents, 4),
        ReadProjectionColumns(intents, 9),
        intents.Text(14),
        DecodeUpdateStatus(intents.Text(15)),
        static_cast<std::size_t>(intents.Integer(16)),
        static_cast<std::size_t>(attempt_count),
        std::move(results),
    });
  }

  Statement outcomes(database,
                     "SELECT request_key FROM logical_scene_request_outcomes");
  while (outcomes.Step()) {
    const auto request_key = outcomes.Key(0);
    if (auto outcome = ReadOutcomeForRequest(database, request_key)) {
      memory.outcomes.emplace(request_key, std::move(*outcome));
    }
  }

  Statement events(database,
                   "SELECT owner_event_sequence,transaction_sequence FROM "
                   "logical_scene_owner_events ORDER BY owner_event_sequence");
  while (events.Step()) {
    memory.owner_events.push_back(SceneOwnerEventRecord{
        static_cast<std::uint64_t>(events.Integer(0)),
        static_cast<std::uint64_t>(events.Integer(1)),
    });
  }
  // The file bytes of every row, sized as the insert that made it sized it.
  // A maintenance row's first column is its initialization sequence.
  for (const auto &table : kSceneRowTables) {
    Statement rows(database, "SELECT * FROM " + std::string(table.name));
    while (rows.Step()) {
      RecordSize size;
      for (int column = 0; column < rows.Columns(); ++column) {
        switch (rows.Type(column)) {
        case SQLITE_INTEGER:
          size.Integer(rows.Integer(column));
          break;
        case SQLITE_NULL:
          size.Null();
          break;
        default:
          size.Text(rows.Bytes(column));
          break;
        }
      }
      const auto bytes = SceneRowFileBytes(size.bytes(), table);
      if (table.maintenance) {
        memory.maintenance_file_bytes[static_cast<std::uint64_t>(
            rows.Integer(0))] += bytes;
      } else {
        memory.transaction_file_bytes += bytes;
      }
    }
  }
  return memory;
}

DurableState ReadCompleteDurableState(const SceneStoreMemory &memory) {
  auto state = ReadDurableState(memory);
  if (!state.finalization_receipt.has_value()) {
    return state;
  }
  const auto transactions = ReadSceneTransactionStoreState(memory, state);
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

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

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
      "maximum_wal_bytes"
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
      maximum_wal_bytes != kMaximumSceneStoreWalBytes) {
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
      "maximum_retained_recoveries "
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

void ValidateDatabase(
    sqlite3 *database,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const ContentDigest &admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
      kCanonicalEmptySceneSqliteApplicationId) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene SQLite application/schema version is invalid");
  }
  // Formats are versioned: a store of another version is refused, never
  // converted. Both launchers create the store inside the run folder, which
  // every start moves to previous-runs/, so a new run opens a new store.
  if (const auto version = ScalarInteger(database, "PRAGMA user_version");
      version != kLogicalSceneStoreSchemaVersion) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene SQLite application/schema version is invalid: the "
        "store has schema version " +
        std::to_string(version) + ", this ManyForge reads only version " +
        std::to_string(kLogicalSceneStoreSchemaVersion) +
        "; it is not converted. Start a new run, which creates a new Scene "
        "store; the old file is kept with its run under previous-runs/");
  }
  ValidateMechanicalDatabaseIntegrity(database);
  if (ReadSchemaObjects(database) != ExpectedSchemaObjects()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene schema differs from its exact v12 artifacts");
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
             history.recovered_through_transaction_sequence != 0U) {
    throw LogicalSceneOwnerIntegrityError(
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
      "maximum_wal_bytes) "
      "VALUES(1,?,?,?,?,?,?,?,?,?,?)");
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
                "maximum_retained_recoveries) VALUES(1,?,?,?,?,?)");
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
  Exec(database, "PRAGMA synchronous=NORMAL");
  if (ScalarInteger(database, "PRAGMA synchronous") != 1) {
    throw LogicalSceneOwnerError(
        "logical-scene database did not enter synchronous NORMAL mode");
  }
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
  if (!intent.execution_host_bindings.empty()) {
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

void InsertAcceptance(persistence::SqliteRecord &statement,
                      const AcceptedState &accepted) {
  statement.Statement(
      "INSERT INTO canonical_empty_scene_acceptance("
      "singleton,bootstrap_id,bootstrap_generation,cell_id,"
      "retained_bundle_id,graph_revision,scene_id,scene_revision_id,"
      "scene_content_hash,transaction_sequence,topology_epoch,policy_epoch,"
      "configured_policy_id,configured_policy_revision_id,"
      "configured_policy_content_hash,object_count,carried_attachment_count,"
      "execution_host_binding_count) "
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
  statement.Bind(15, std::int64_t{0});
  statement.Bind(16, std::int64_t{0});
  statement.Bind(17, std::int64_t{0});
}

void InsertRebuild(persistence::SqliteRecord &statement,
                   const AcceptedState &accepted) {
  statement.Statement(
      "INSERT INTO canonical_empty_scene_rebuild("
      "singleton,bootstrap_id,bootstrap_generation,"
      "native_rebuild_result_count,aggregate_health_state,"
      "aggregate_native_health_count) VALUES(1,?,?,0,'healthy',0)");
  statement.Bind(1, accepted.bootstrap_id.value());
  statement.Bind(2, CheckedSafeCounter(accepted.bootstrap_generation.value(),
                                       "bootstrap_generation"));
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

// The finalization row a Finalize request stores.
FinalizationColumns
FinalizationRow(const ProjectionBootstrapFinalizeDTO &finalize) {
  const auto *transaction = std::get_if<BootstrapPublicationTransactionDTO>(
      &finalize.publication_receipt.request.transaction_ref);
  if (transaction == nullptr) {
    throw LogicalSceneOwnerConflictError(
        "canonical-empty Finalize receipt is not bootstrap-tagged");
  }
  const auto &receipt = finalize.publication_receipt;
  const auto &publication_request = receipt.request;
  const auto &target = publication_request.candidate_projection_revision;
  return {
      finalize.bootstrap_id.value(),
      CheckedSafeCounter(finalize.bootstrap_generation.value(),
                         "bootstrap_generation"),
      CheckedSafeCounter(receipt.publication_generation.value(),
                         "publication_generation"),
      transaction->bootstrap_id.value(),
      CheckedSafeCounter(transaction->bootstrap_generation.value(),
                         "publication bootstrap_generation"),
      target.retained_bundle_id.value(),
      target.graph_revision.value(),
      target.scene_revision.scene_id.value(),
      static_cast<std::int64_t>(target.scene_revision.revision_id),
      target.scene_revision.content_hash.value(),
      publication_request.candidate_retained_bundle_id.value(),
      publication_request.candidate_graph_revision.value(),
      publication_request.private_materialization_content_digest.value(),
      publication_request.private_materialization_content_digest.value(),
      receipt.durable_public_pointer_content_digest.value(),
  };
}

void InsertFinalization(persistence::SqliteRecord &statement,
                        const FinalizationColumns &row) {
  statement.Statement(
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
  statement.Bind(1, row.bootstrap_id);
  statement.Bind(2, row.bootstrap_generation);
  statement.Bind(3, row.publication_generation);
  statement.Bind(4, row.publication_bootstrap_id);
  statement.Bind(5, row.publication_bootstrap_generation);
  statement.Bind(6, row.candidate_retained_bundle_id);
  statement.Bind(7, row.candidate_graph_revision);
  statement.Bind(8, row.candidate_scene_id);
  statement.Bind(9, row.candidate_scene_revision_id);
  statement.Bind(10, row.candidate_scene_content_hash);
  statement.Bind(11, row.request_retained_bundle_id);
  statement.Bind(12, row.request_graph_revision);
  statement.Bind(13, row.private_materialization_digest);
  statement.Bind(14, row.compatibility_materialization_digest);
  statement.Bind(15, row.public_pointer_digest);
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
  // The newest row is open while it has no receipt and no result other than
  // `initialized`.
  const bool maintenance_open =
      maintenance != nullptr && !maintenance->publication_receipt.has_value() &&
      (!maintenance->result_state.has_value() ||
       *maintenance->result_state ==
           ProjectionMaintenanceInitializationState::kInitialized);
  const bool transition_open = maintenance_open;
  const bool projection_available = !state.projection_desynchronized &&
                                    !state.unresolved_scene_transaction;
  // An ended newest row takes no part: the phase is the one before it.
  const bool any_completed = std::ranges::any_of(
      state.maintenance_initializations,
      [](const MaintenanceInitializationState &row) {
        return row.publication_receipt.has_value();
      });
  return {
      maintenance == nullptr
          ? state.phase()
          : (maintenance_open
                 ? CanonicalEmptySceneBootstrapPhase::kMaintenanceInitializing
                 : (any_completed
                        ? CanonicalEmptySceneBootstrapPhase::
                              kMaintenanceCompleted
                        : state.phase())),
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

LogicalSceneOwner::LogicalSceneOwner(
    std::filesystem::path database_path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
    const core::IPlannerSceneProjectionResolver &projection_resolver,
    core::IKernelCellAdmissionPort &cell_admission,
    persistence::SqliteRecordWriter *record_writer)
    : impl_(std::make_unique<Impl>(
          std::move(database_path), admission, publication_reader,
          retained_bundle_source, projection_resolver, cell_admission,
          record_writer)) {}

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
  impl_->RequireOpen();
  const auto state = ReadCompleteDurableState(impl_->memory_);
  if (state.accepted.has_value()) {
    if (state.accepted->bootstrap_id != intent.bootstrap_id ||
        state.accepted->bootstrap_generation != intent.bootstrap_generation) {
      throw LogicalSceneOwnerConflictError(
          "a different canonical-empty bootstrap already owns this scene "
          "lineage");
    }
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
  persistence::SqliteRecord record;
  InsertAcceptance(record, accepted);
  impl_->memory_.accepted = accepted;
  impl_->Post(std::move(record));
  return AcceptedResult(accepted);
}

ProjectionBootstrapRebuildResultDTO
LogicalSceneOwner::RebuildProjectionBootstrap(
    const ProjectionBootstrapRebuildRequestDTO &request) {
  if (!request.valid()) {
    throw std::invalid_argument("bootstrap Rebuild request is invalid");
  }
  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  impl_->RequireOpen();
  const auto state = ReadCompleteDurableState(impl_->memory_);
  if (!state.accepted.has_value() ||
      request.bootstrap_id != state.accepted->bootstrap_id ||
      request.bootstrap_generation != state.accepted->bootstrap_generation ||
      request.canonical_empty_target != state.accepted->target) {
    throw LogicalSceneOwnerConflictError(
        "Rebuild does not match the exact durable Begin acceptance");
  }
  persistence::SqliteRecord record;
  if (!state.rebuilt) {
    InsertRebuild(record, *state.accepted);
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
  if (!state.rebuilt) {
    impl_->memory_.rebuilt = true;
    impl_->Post(std::move(record));
  }
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

  impl_->RequireOpen();
  state = ReadCompleteDurableState(impl_->memory_);
  if (!state.accepted.has_value() || !state.rebuilt) {
    throw LogicalSceneOwnerIntegrityError(
        "durable bootstrap state changed before Finalize commit");
  }
  ValidateFinalizeRequest(request, *state.accepted);
  persistence::SqliteRecord record;
  if (!state.finalization_receipt.has_value()) {
    // The receipt is the one the stored row stands for, as the file
    // returned it.
    const auto row = FinalizationRow(request);
    InsertFinalization(record, row);
    state.finalization_receipt =
        FinalizationReceipt(row, state.accepted, state.rebuilt);
  }
  if (!state.finalization_receipt.has_value() ||
      *state.finalization_receipt != request.publication_receipt) {
    throw LogicalSceneOwnerIntegrityError(
        "Finalize receipt was not durably recorded exactly");
  }
  if (!record.empty()) {
    impl_->memory_.finalization_receipt = state.finalization_receipt;
    impl_->Post(std::move(record));
  }

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
  const ContentDigest digest(manyforge::content_identity::sha256_digest(
      logical_scene_codec_internal::EncodeRequestDigestInput(request)));
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  const auto result = ReadOutcomeForRequest(
      impl_->memory_, request.request_context.request_key);
  if (!result.has_value())
    return std::nullopt;
  if (result->request_digest != digest)
    throw LogicalSceneOwnerConflictError(
        "scene result lookup changed the exact request");
  return ResultFromStoredOutcome(impl_->memory_, *result,
                                 core::RequestReplayStatusDTO::kReplayedExact);
}

std::uint64_t LogicalSceneOwner::record_file_bytes() const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  return SceneFileBytes(impl_->memory_);
}

// Reports the memory rows. The file's own checks (schema, metadata,
// quick_check, page ceiling) run when the store is opened.
CanonicalEmptySceneStoreIntegrityReport
LogicalSceneOwner::VerifyIntegrity() const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  const auto &memory = impl_->memory_;
  const auto state = ReadCompleteDurableState(memory);
  std::int64_t attempt_count = 0;
  std::int64_t result_count = 0;
  for (const auto &transaction : memory.transactions) {
    attempt_count +=
        static_cast<std::int64_t>(transaction.projection_attempt_count);
    result_count +=
        static_cast<std::int64_t>(transaction.projection_results.size());
  }
  CanonicalEmptySceneStoreIntegrityReport report{
      impl_->fixed_query_,
      std::string(kLogicalSceneStoreSchemaId),
      kLogicalSceneStoreSchemaVersion,
      CurrentSchemaArtifactDigest(),
      memory.accepted.has_value() ? 1 : 0,
      memory.rebuilt ? 1 : 0,
      memory.finalization_receipt.has_value() ? 1 : 0,
      static_cast<std::int64_t>(memory.transactions.size()),
      static_cast<std::int64_t>(memory.outcomes.size()),
      attempt_count,
      result_count,
      static_cast<std::int64_t>(memory.owner_events.size()),
      state.projection_desynchronized || state.unresolved_scene_transaction,
      AccessStatus(state),
  };
  if (!report.access.valid() || report.acceptance_row_count < 0 ||
      report.acceptance_row_count > 1 || report.rebuild_row_count < 0 ||
      report.rebuild_row_count > 1 || report.finalization_row_count < 0 ||
      report.finalization_row_count > 1 ||
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
