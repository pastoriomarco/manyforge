#include "manyforge_scene/canonical_empty_scene_owner.hpp"

#include "embedded_canonical_empty_scene_schema.hpp"

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_persistence/sqlite_read_only_preflight.hpp>

#include <sqlite3.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if SQLITE_VERSION_NUMBER < 3037000
#error "canonical-empty scene owner requires SQLite 3.37.0 or newer"
#endif

#ifndef SQLITE_OPEN_NOFOLLOW
#error "canonical-empty scene owner requires SQLITE_OPEN_NOFOLLOW"
#endif

#include <algorithm>
#include <cerrno>
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
using core::ProjectionAggregateHealthDTO;
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
using core::RetainedBundleId;
using core::RetainedBundleDTO;
using core::SceneId;
using core::SceneRevision;

constexpr int kBusyTimeoutMilliseconds = 10000;
constexpr std::int64_t kPageSizeBytes = 4096;
constexpr std::int64_t kMaximumPageCount =
    kMaximumSceneStoreDatabasePageImageBytes / kPageSizeBytes;
constexpr std::int64_t kExpectedSchemaObjectCardinality = 12;
constexpr std::uint64_t kMaximumSafeIntegral = 9007199254740991ULL;
constexpr int kMaximumSqliteValueBytes = 1024 * 1024;

[[noreturn]] void ThrowSqlite(
    sqlite3* database,
    const int result,
    const std::string_view context) {
  const std::string detail =
      database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
  if (result == SQLITE_FULL || result == SQLITE_TOOBIG ||
      result == SQLITE_NOMEM) {
    throw CanonicalEmptySceneOwnerLimitError(
        std::string(context) + ": storage limit exhausted: " + detail);
  }
  if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
    throw CanonicalEmptySceneOwnerBusyError(
        std::string(context) + ": " + detail);
  }
  if (result == SQLITE_CONSTRAINT) {
    throw CanonicalEmptySceneOwnerConflictError(
        std::string(context) + ": " + detail);
  }
  if (result == SQLITE_CORRUPT || result == SQLITE_NOTADB ||
      result == SQLITE_SCHEMA) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        std::string(context) + ": " + detail);
  }
  throw CanonicalEmptySceneOwnerError(
      std::string(context) + ": " + detail);
}

void Check(
    sqlite3* database,
    const int result,
    const std::string_view context) {
  if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW) {
    ThrowSqlite(database, result, context);
  }
}

void Exec(sqlite3* database, const std::string_view sql) {
  char* error = nullptr;
  const auto result = sqlite3_exec(
      database, std::string(sql).c_str(), nullptr, nullptr, &error);
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
  Statement(sqlite3* database, const std::string_view sql)
      : database_(database) {
    Check(
        database_,
        sqlite3_prepare_v2(
            database_, std::string(sql).c_str(), -1, &statement_, nullptr),
        "prepare SQLite statement");
  }

  ~Statement() {
    if (statement_ != nullptr) {
      sqlite3_finalize(statement_);
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void Bind(const int index, const std::string_view value) {
    Check(
        database_,
        sqlite3_bind_text(
            statement_, index, value.data(),
            static_cast<int>(value.size()), SQLITE_TRANSIENT),
        "bind SQLite text");
  }

  void Bind(const int index, const std::int64_t value) {
    Check(
        database_, sqlite3_bind_int64(statement_, index, value),
        "bind SQLite integer");
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
    const auto* value = sqlite3_column_text(statement_, column);
    if (value == nullptr) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "stored logical-scene text is null");
    }
    return {
        reinterpret_cast<const char*>(value),
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

 private:
  sqlite3* database_;
  sqlite3_stmt* statement_{};
};

class Transaction final {
 public:
  Transaction(sqlite3* database, const std::string_view begin)
      : database_(database) {
    Exec(database_, begin);
  }

  ~Transaction() {
    if (!finished_) {
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
  }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  void Commit() {
    Exec(database_, "COMMIT");
    finished_ = true;
  }

 private:
  sqlite3* database_;
  bool finished_{};
};

std::int64_t ScalarInteger(sqlite3* database, const std::string_view sql) {
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

std::string ScalarText(sqlite3* database, const std::string_view sql) {
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

SchemaObjects ReadSchemaObjects(sqlite3* database) {
  Statement statement(
      database,
      "SELECT type,name,sql FROM sqlite_master "
      "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name LIMIT 13");
  SchemaObjects objects;
  std::int64_t count = 0;
  while (statement.Step()) {
    ++count;
    if (count > kExpectedSchemaObjectCardinality) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene schema object cardinality exceeds v1");
    }
    objects.emplace(
        std::make_pair(statement.Text(0), statement.Text(1)),
        NormalizedSql(statement.Text(2)));
  }
  return objects;
}

const SchemaObjects& ExpectedSchemaObjects() {
  static const SchemaObjects expected = [] {
    sqlite3* database = nullptr;
    const auto result = sqlite3_open_v2(
        ":memory:", &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (result != SQLITE_OK) {
      const std::string detail =
          database == nullptr ? sqlite3_errstr(result) :
                                sqlite3_errmsg(database);
      if (database != nullptr) {
        sqlite3_close(database);
      }
      throw CanonicalEmptySceneOwnerError(
          "cannot open in-memory scene schema validator: " + detail);
    }
    try {
      Exec(database, detail::kEmbeddedCanonicalEmptySceneSchema);
      auto objects = ReadSchemaObjects(database);
      if (objects.size() !=
          static_cast<std::size_t>(kExpectedSchemaObjectCardinality)) {
        throw CanonicalEmptySceneOwnerError(
            "embedded logical-scene schema cardinality is invalid");
      }
      sqlite3_close(database);
      return objects;
    } catch (...) {
      sqlite3_close(database);
      throw;
    }
  }();
  return expected;
}

void ValidateBoundedIdentity(
    const std::string_view value,
    const std::string_view label) {
  if (value.empty() || value.size() > kMaximumSceneStoreIdentifierBytes ||
      std::any_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x21U || byte > 0x7EU;
      })) {
    throw std::invalid_argument(
        std::string(label) +
        " must be bounded non-empty printable ASCII");
  }
}

void ValidateDigest(
    const std::string_view value,
    const std::string_view label) {
  if (!core::IsExactLowercaseSha256Digest(value)) {
    throw std::invalid_argument(
        std::string(label) + " must be an exact lowercase SHA-256 digest");
  }
}

std::int64_t CheckedSafeCounter(
    const std::uint64_t value,
    const std::string_view label) {
  if (value == 0U || value > kMaximumSafeIntegral) {
    throw std::invalid_argument(
        std::string(label) +
        " must fit the nonzero RFC-8785 safe-integral range");
  }
  return static_cast<std::int64_t>(value);
}

void ValidateFixedQuery(const ProjectionBootstrapPrerequisiteQueryDTO& query) {
  if (!query.valid()) {
    throw std::invalid_argument(
        "canonical-empty scene prerequisite query is invalid");
  }
  ValidateBoundedIdentity(query.cell_id.value(), "cell_id");
  ValidateDigest(
      query.canonical_empty_retained_bundle_id.value(),
      "canonical_empty_retained_bundle_id");
  ValidateDigest(
      query.canonical_empty_graph_revision.value(),
      "canonical_empty_graph_revision");
}

struct AdmittedBootstrapAuthority final {
  ProjectionBootstrapPrerequisiteQueryDTO query;
  ContentDigest admission_digest;
  RetainedBundleDTO retained_bundle;
};

AdmittedBootstrapAuthority AuthorityFromAdmission(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  const auto& retained = admission.retained_bundle();
  const auto& bundle = retained.value();
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
  ValidateDigest(
      admission.admission_digest().value(),
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

std::string SchemaArtifactDigest() {
  return manyforge::content_identity::sha256_digest(
      detail::kEmbeddedCanonicalEmptySceneSchema);
}

struct AcceptedState final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionRevisionDTO target;
  LogicalSceneSnapshotDTO snapshot;
};

struct DurableState final {
  std::optional<AcceptedState> accepted;
  bool rebuilt{};
  std::optional<ActiveBundlePublicationReceiptDTO> finalization_receipt;

  [[nodiscard]] CanonicalEmptySceneBootstrapPhase phase() const noexcept {
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

std::optional<AcceptedState> ReadAcceptance(
    sqlite3* database,
    const ProjectionBootstrapPrerequisiteQueryDTO& fixed_query) {
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
      topology_epoch != 0 || policy_epoch != 0 ||
      policy_id != "free_space" || policy_revision_id != 1 ||
      policy_content_hash != core::kCanonicalEmptyCollisionPolicyDigest ||
      object_count != 0 || attachment_count != 0 ||
      native_binding_count != 0) {
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
  policy.revision = {
      static_cast<std::uint64_t>(policy_revision_id), policy_content_hash};
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
    const auto expected_scene_id =
        core::logical_scene_identity::MintSceneId(
            LogicalSceneBootstrapIdentityInputDTO{
                fixed_query.cell_id, bootstrap_id, bootstrap_generation});
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
  } catch (const CanonicalEmptySceneOwnerIntegrityError&) {
    throw;
  } catch (const std::exception& error) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot verify stored canonical-empty scene identity: " +
        std::string(error.what()));
  }

  return AcceptedState{
      bootstrap_id, bootstrap_generation, target, std::move(snapshot)};
}

bool ReadRebuild(
    sqlite3* database,
    const std::optional<AcceptedState>& accepted) {
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
  if (!accepted.has_value() ||
      bootstrap_id != accepted->bootstrap_id.value() ||
      bootstrap_generation !=
          static_cast<std::int64_t>(
              accepted->bootstrap_generation.value()) ||
      rebuild_count != 0 || health_state != "healthy" ||
      health_count != 0) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored zero-native rebuild does not match its acceptance");
  }
  return true;
}

std::optional<ActiveBundlePublicationReceiptDTO> ReadFinalization(
    sqlite3* database,
    const std::optional<AcceptedState>& accepted,
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
      candidate_scene_id !=
          accepted->target.scene_revision.scene_id.value() ||
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
  ValidateDigest(
      private_materialization_digest,
      "stored private materialization digest");
  ValidateDigest(
      retired_surfaces_digest,
      "stored retired command-surfaces digest");
  ValidateDigest(public_pointer_digest, "stored public-pointer digest");

  ActiveBundlePublicationRequestDTO request{
      BootstrapPublicationTransactionDTO{
          BootstrapId(publication_bootstrap_id),
          BootstrapGeneration(
              static_cast<std::uint64_t>(
                  publication_bootstrap_generation)),
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
  if (!receipt.matches_bootstrap(
          accepted->bootstrap_id, accepted->bootstrap_generation,
          accepted->target)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "stored finalization receipt is structurally invalid");
  }
  return receipt;
}

DurableState ReadDurableState(
    sqlite3* database,
    const ProjectionBootstrapPrerequisiteQueryDTO& fixed_query) {
  DurableState state;
  state.accepted = ReadAcceptance(database, fixed_query);
  state.rebuilt = ReadRebuild(database, state.accepted);
  state.finalization_receipt =
      ReadFinalization(database, state.accepted, state.rebuilt);
  return state;
}

void ValidateMetadata(
    sqlite3* database,
    const ProjectionBootstrapPrerequisiteQueryDTO& fixed_query,
    const ContentDigest& admission_digest) {
  Statement statement(
      database,
      "SELECT schema_id,schema_version,schema_artifact_digest,"
      "canonical_empty_admission_digest,cell_id,"
      "canonical_empty_retained_bundle_id,canonical_empty_graph_revision,"
      "maximum_identifier_bytes,maximum_database_page_image_bytes,"
      "maximum_wal_bytes FROM logical_scene_store_metadata "
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
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene metadata cardinality exceeds one");
  }
  if (schema_id != kCanonicalEmptySceneStoreSchemaId ||
      schema_version != kCanonicalEmptySceneStoreSchemaVersion ||
      schema_digest != SchemaArtifactDigest() ||
      stored_admission_digest != admission_digest.value() ||
      cell_id != fixed_query.cell_id.value() ||
      retained_bundle_id !=
          fixed_query.canonical_empty_retained_bundle_id.value() ||
      graph_revision !=
          fixed_query.canonical_empty_graph_revision.value() ||
      maximum_identifier_bytes !=
          static_cast<std::int64_t>(kMaximumSceneStoreIdentifierBytes) ||
      maximum_database_bytes !=
          kMaximumSceneStoreDatabasePageImageBytes ||
      maximum_wal_bytes != kMaximumSceneStoreWalBytes) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene metadata does not match the fixed v1 owner");
  }
}

void ValidateDatabase(
    sqlite3* database,
    const ProjectionBootstrapPrerequisiteQueryDTO& fixed_query,
    const ContentDigest& admission_digest) {
  if (ScalarInteger(database, "PRAGMA application_id") !=
          kCanonicalEmptySceneSqliteApplicationId ||
      ScalarInteger(database, "PRAGMA user_version") !=
          kCanonicalEmptySceneStoreSchemaVersion) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene SQLite application/schema version is invalid");
  }
  if (ScalarText(database, "PRAGMA quick_check(1)") != "ok") {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene SQLite quick_check failed");
  }
  Statement foreign_keys(database, "PRAGMA foreign_key_check");
  if (foreign_keys.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene SQLite foreign-key check failed");
  }
  if (ReadSchemaObjects(database) != ExpectedSchemaObjects()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene schema differs from its exact v1 artifact");
  }
  const auto page_count = ScalarInteger(database, "PRAGMA page_count");
  const auto page_size = ScalarInteger(database, "PRAGMA page_size");
  if (page_count < 0 || page_size <= 0 ||
      page_count >
          kMaximumSceneStoreDatabasePageImageBytes / page_size) {
    throw CanonicalEmptySceneOwnerLimitError(
        "logical-scene database page image exceeds its fixed ceiling");
  }
  ValidateMetadata(database, fixed_query, admission_digest);
  static_cast<void>(ReadDurableState(database, fixed_query));
}

void InsertMetadata(
    sqlite3* database,
    const ProjectionBootstrapPrerequisiteQueryDTO& fixed_query,
    const ContentDigest& admission_digest) {
  Statement statement(
      database,
      "INSERT INTO logical_scene_store_metadata("
      "singleton,schema_id,schema_version,schema_artifact_digest,"
      "canonical_empty_admission_digest,cell_id,"
      "canonical_empty_retained_bundle_id,canonical_empty_graph_revision,"
      "maximum_identifier_bytes,maximum_database_page_image_bytes,"
      "maximum_wal_bytes) VALUES(1,?,?,?,?,?,?,?,?,?,?)");
  statement.Bind(1, kCanonicalEmptySceneStoreSchemaId);
  statement.Bind(2, kCanonicalEmptySceneStoreSchemaVersion);
  statement.Bind(3, SchemaArtifactDigest());
  statement.Bind(4, admission_digest.value());
  statement.Bind(5, fixed_query.cell_id.value());
  statement.Bind(6, fixed_query.canonical_empty_retained_bundle_id.value());
  statement.Bind(7, fixed_query.canonical_empty_graph_revision.value());
  statement.Bind(
      8, static_cast<std::int64_t>(kMaximumSceneStoreIdentifierBytes));
  statement.Bind(9, kMaximumSceneStoreDatabasePageImageBytes);
  statement.Bind(10, kMaximumSceneStoreWalBytes);
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "metadata insert unexpectedly returned a row");
  }
}

void ConfigureConnection(sqlite3* database) {
  if (sqlite3_db_config(
          database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) !=
          SQLITE_OK ||
      sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) !=
          SQLITE_OK) {
    throw CanonicalEmptySceneOwnerError(
        "cannot configure defensive logical-scene SQLite connection");
  }
  Check(
      database, sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds),
      "configure SQLite busy timeout");
  sqlite3_extended_result_codes(database, 1);
  sqlite3_limit(database, SQLITE_LIMIT_LENGTH, kMaximumSqliteValueBytes);
  sqlite3_limit(database, SQLITE_LIMIT_SQL_LENGTH, kMaximumSqliteValueBytes);
  sqlite3_limit(database, SQLITE_LIMIT_ATTACHED, 0);
  Exec(database, "PRAGMA foreign_keys=ON");
}

void ConfigureDurability(sqlite3* database) {
  Exec(database, "PRAGMA page_size=4096");
  if (ScalarText(database, "PRAGMA journal_mode=WAL") != "wal") {
    throw CanonicalEmptySceneOwnerError(
        "logical-scene database did not enter WAL mode");
  }
  Exec(database, "PRAGMA synchronous=FULL");
  Exec(database, "PRAGMA wal_autocheckpoint=1000");
  Exec(database, "PRAGMA journal_size_limit=16777216");
  Statement maximum_pages(database, "PRAGMA max_page_count=16384");
  if (!maximum_pages.Step() ||
      maximum_pages.Integer(0) > kMaximumPageCount ||
      maximum_pages.Step()) {
    throw CanonicalEmptySceneOwnerLimitError(
        "cannot enforce logical-scene database page ceiling");
  }
}

bool IsExactUninitializedDatabase(sqlite3* database) {
  return ScalarInteger(
             database,
             "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE "
             "'sqlite_%'") == 0 &&
         ScalarInteger(database, "PRAGMA application_id") == 0 &&
         ScalarInteger(database, "PRAGMA user_version") == 0;
}

void FsyncDatabaseDirectory(const std::filesystem::path& database_path) {
  auto directory = database_path.parent_path();
  if (directory.empty()) {
    directory = ".";
  }
  const int descriptor = open(
      directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
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
    const ProjectionBootstrapIntentDTO& intent,
    const ProjectionBootstrapPrerequisiteQueryDTO& fixed_query,
    const RetainedBundleDTO& admitted_bundle) {
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
  static_cast<void>(CheckedSafeCounter(
      intent.bootstrap_generation.value(), "bootstrap_generation"));
  const auto& bundle = intent.canonical_empty_retained_bundle;
  const auto& graph = bundle.cell_graph;
  if (intent.cell_id != fixed_query.cell_id ||
      bundle.api_version != core::kRetainedBundleApiVersion ||
      bundle.retained_bundle_id !=
          fixed_query.canonical_empty_retained_bundle_id ||
      bundle != admitted_bundle ||
      graph.api_version != "manyforge.cell_graph.v1" ||
      graph.cell_id != fixed_query.cell_id ||
      graph.graph_revision !=
          fixed_query.canonical_empty_graph_revision ||
      !graph.runtime_units.empty() || !graph.model_instances.empty() ||
      !graph.controllable_components.empty() ||
      !graph.control_domains.empty() || !graph.control_bindings.empty() ||
      !graph.component_attachments.empty() ||
      !bundle.desired_control_configurations.empty() ||
      !bundle.capability_provider_bindings.empty() ||
      bundle.compiled_products.viewer_model.has_value() ||
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
  ValidateDigest(
      bundle.compiled_products.planning_collision_model
          .product_contract_revision.value(),
      "planning collision model contract revision");
}

AcceptedState MintAcceptedState(
    const ProjectionBootstrapIntentDTO& intent,
    const ProjectionBootstrapPrerequisiteQueryDTO& fixed_query) {
  try {
    const auto scene_id = core::logical_scene_identity::MintSceneId(
        LogicalSceneBootstrapIdentityInputDTO{
            fixed_query.cell_id, intent.bootstrap_id,
            intent.bootstrap_generation});
    CollisionPolicySnapshot policy;
    policy.revision = {
        1U, std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
    LogicalSceneSnapshotDTO snapshot{
        SceneRevision{
            scene_id,
            1U,
            ContentDigest(
                "sha256:0000000000000000000000000000000000000000000000000000000000000000"),
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
  } catch (const CanonicalEmptySceneOwnerError&) {
    throw;
  } catch (const std::exception& error) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot mint canonical-empty logical-scene identity: " +
        std::string(error.what()));
  }
}

void InsertAcceptance(sqlite3* database, const AcceptedState& accepted) {
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
  statement.Bind(
      2, CheckedSafeCounter(
             accepted.bootstrap_generation.value(), "bootstrap_generation"));
  statement.Bind(3, accepted.snapshot.cell_id.value());
  statement.Bind(4, accepted.target.retained_bundle_id.value());
  statement.Bind(5, accepted.target.graph_revision.value());
  statement.Bind(6, accepted.target.scene_revision.scene_id.value());
  statement.Bind(
      7, static_cast<std::int64_t>(
             accepted.target.scene_revision.revision_id));
  statement.Bind(8, accepted.target.scene_revision.content_hash.value());
  statement.Bind(
      9, static_cast<std::int64_t>(accepted.snapshot.transaction_sequence));
  statement.Bind(
      10, static_cast<std::int64_t>(accepted.snapshot.topology_epoch));
  statement.Bind(
      11, static_cast<std::int64_t>(accepted.snapshot.policy_epoch));
  statement.Bind(12, accepted.snapshot.configured_collision_policy.policy_id);
  statement.Bind(
      13, static_cast<std::int64_t>(
              accepted.snapshot.configured_collision_policy.revision
                  .revision_id));
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

void InsertRebuild(sqlite3* database, const AcceptedState& accepted) {
  Statement statement(
      database,
      "INSERT INTO canonical_empty_scene_rebuild("
      "singleton,bootstrap_id,bootstrap_generation,"
      "native_rebuild_result_count,aggregate_health_state,"
      "aggregate_native_health_count) VALUES(1,?,?,0,'healthy',0)");
  statement.Bind(1, accepted.bootstrap_id.value());
  statement.Bind(
      2, CheckedSafeCounter(
             accepted.bootstrap_generation.value(), "bootstrap_generation"));
  if (statement.Step()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "rebuild insert unexpectedly returned a row");
  }
}

void ValidateFinalizeRequest(
    const ProjectionBootstrapFinalizeDTO& request,
    const AcceptedState& accepted) {
  if (!request.valid() ||
      request.bootstrap_id != accepted.bootstrap_id ||
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
  ValidateDigest(
      request.publication_receipt.request
          .private_materialization_content_digest.value(),
      "private materialization content digest");
  ValidateDigest(
      request.publication_receipt.request
          .obsolete_managed_command_surfaces_retired_evidence_digest.value(),
      "retired command-surfaces evidence digest");
  ValidateDigest(
      request.publication_receipt.durable_public_pointer_content_digest
          .value(),
      "durable public-pointer content digest");
}

void InsertFinalization(
    sqlite3* database,
    const ProjectionBootstrapFinalizeDTO& finalize) {
  const auto* transaction =
      std::get_if<BootstrapPublicationTransactionDTO>(
          &finalize.publication_receipt.request.transaction_ref);
  if (transaction == nullptr) {
    throw CanonicalEmptySceneOwnerConflictError(
        "canonical-empty Finalize receipt is not bootstrap-tagged");
  }
  const auto& receipt = finalize.publication_receipt;
  const auto& publication_request = receipt.request;
  const auto& target = publication_request.candidate_projection_revision;
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
  statement.Bind(
      2, CheckedSafeCounter(
             finalize.bootstrap_generation.value(), "bootstrap_generation"));
  statement.Bind(
      3, CheckedSafeCounter(
             receipt.publication_generation.value(),
             "publication_generation"));
  statement.Bind(4, transaction->bootstrap_id.value());
  statement.Bind(
      5, CheckedSafeCounter(
             transaction->bootstrap_generation.value(),
             "publication bootstrap_generation"));
  statement.Bind(6, target.retained_bundle_id.value());
  statement.Bind(7, target.graph_revision.value());
  statement.Bind(8, target.scene_revision.scene_id.value());
  statement.Bind(
      9, static_cast<std::int64_t>(target.scene_revision.revision_id));
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

ProjectionAggregateHealthDTO ZeroNativeHealth(
    const AcceptedState& accepted) {
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

ProjectionBootstrapEvidenceDTO ZeroNativeEvidence(
    const AcceptedState& accepted) {
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.target,
      {},
      ZeroNativeHealth(accepted),
  };
}

ProjectionBootstrapAcceptedDTO AcceptedResult(const AcceptedState& accepted) {
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.snapshot.cell_id,
      accepted.target,
      LogicalSceneSnapshotRoot(
          std::make_shared<const LogicalSceneSnapshotDTO>(
              accepted.snapshot)),
  };
}

CanonicalEmptySceneAccessStatus AccessStatus(const DurableState& state) {
  const bool finalized = state.finalization_receipt.has_value();
  return {
      state.phase(),
      finalized,
      finalized,
      finalized ? std::optional<ProjectionRevisionDTO>(
                      state.accepted->target) :
                  std::nullopt,
  };
}

ProjectionBootstrapStatusDTO StatusResult(
    const DurableState& state,
    const ProjectionBootstrapStatusQueryDTO& query) {
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
  const auto& accepted = *state.accepted;
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

ProjectionBootstrapFinalizeResultDTO FailedFinalizeResult(
    const ProjectionBootstrapFinalizeDTO& request,
    const AcceptedState& accepted,
    const ProjectionBootstrapFinalizeResultState state,
    std::string code,
    std::string message) {
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

}  // namespace

std::string_view ToString(
    const CanonicalEmptySceneBootstrapPhase phase) noexcept {
  switch (phase) {
    case CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced:
      return "initial_command_fenced";
    case CanonicalEmptySceneBootstrapPhase::kAccepted:
      return "accepted";
    case CanonicalEmptySceneBootstrapPhase::kReadyToPublish:
      return "ready_to_publish";
    case CanonicalEmptySceneBootstrapPhase::kFinalized:
      return "finalized";
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
      return scene_writes_open && projection_leases_open &&
             active_projection.has_value() && active_projection->valid();
  }
  return false;
}

class CanonicalEmptySceneOwner::Impl final {
 public:
  Impl(
      std::filesystem::path database_path,
      const deployment::CanonicalEmptyRetainedBundleAdmission& admission,
      const core::IActiveBundlePublicationReader& publication_reader)
      : Impl(
            std::move(database_path), AuthorityFromAdmission(admission),
            publication_reader) {}

  Impl(
      std::filesystem::path database_path,
      AdmittedBootstrapAuthority authority,
      const core::IActiveBundlePublicationReader& publication_reader)
      : database_path_(std::move(database_path)),
        fixed_query_(std::move(authority.query)),
        admission_digest_(std::move(authority.admission_digest)),
        admitted_bundle_(std::move(authority.retained_bundle)),
        publication_reader_(publication_reader) {
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

    try {
      static_cast<void>(
          manyforge::persistence::PreflightExistingSqliteReadOnly(
              database_path_,
              {
                  static_cast<std::uintmax_t>(
                      kMaximumSceneStoreDatabasePageImageBytes),
                  static_cast<std::uintmax_t>(kMaximumSceneStoreWalBytes),
              },
              [&](sqlite3* preflight) {
                if (!IsExactUninitializedDatabase(preflight)) {
                  ValidateDatabase(
                      preflight, fixed_query_, admission_digest_);
                }
              }));
    } catch (const CanonicalEmptySceneOwnerError&) {
      throw;
    } catch (const std::exception& error) {
      throw CanonicalEmptySceneOwnerIntegrityError(
          "logical-scene read-only startup preflight failed: " +
          std::string(error.what()));
    }

    const auto result = sqlite3_open_v2(
        database_path_.c_str(), &database_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX |
            SQLITE_OPEN_NOFOLLOW,
        nullptr);
    if (result != SQLITE_OK) {
      const std::string detail =
          database_ == nullptr ? sqlite3_errstr(result) :
                                 sqlite3_errmsg(database_);
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
          database_,
          "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE "
          "'sqlite_%'");
      bool initialized_schema = false;
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
        transaction.Commit();
        initialized_schema = true;
      } else {
        ValidateDatabase(database_, fixed_query_, admission_digest_);
        ConfigureDurability(database_);
      }
      ValidateDatabase(database_, fixed_query_, admission_digest_);
      if (initialized_schema) {
        FsyncDatabaseDirectory(database_path_);
      }
    } catch (...) {
      sqlite3_close(database_);
      database_ = nullptr;
      throw;
    }
  }

  ~Impl() {
    if (database_ != nullptr) {
      sqlite3_close(database_);
    }
  }

  [[nodiscard]] sqlite3* database() const {
    if (database_ == nullptr) {
      throw CanonicalEmptySceneOwnerClosedError(
          "logical-scene owner is closed");
    }
    return database_;
  }

  [[nodiscard]] DurableState ReadStateTransaction() const {
    auto* value = database();
    Transaction transaction(value, "BEGIN");
    auto state = ReadDurableState(value, fixed_query_);
    transaction.Commit();
    return state;
  }

  std::filesystem::path database_path_;
  ProjectionBootstrapPrerequisiteQueryDTO fixed_query_;
  ContentDigest admission_digest_;
  RetainedBundleDTO admitted_bundle_;
  const core::IActiveBundlePublicationReader& publication_reader_;
  mutable std::mutex mutex_;
  sqlite3* database_{};
};

CanonicalEmptySceneOwner::CanonicalEmptySceneOwner(
    std::filesystem::path database_path,
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission,
    const core::IActiveBundlePublicationReader& publication_reader)
    : impl_(std::make_unique<Impl>(
          std::move(database_path), admission, publication_reader)) {}

CanonicalEmptySceneOwner::~CanonicalEmptySceneOwner() = default;
CanonicalEmptySceneOwner::CanonicalEmptySceneOwner(
    CanonicalEmptySceneOwner&&) noexcept = default;
CanonicalEmptySceneOwner& CanonicalEmptySceneOwner::operator=(
    CanonicalEmptySceneOwner&&) noexcept = default;

const std::filesystem::path& CanonicalEmptySceneOwner::database_path()
    const noexcept {
  return impl_->database_path_;
}

const ProjectionBootstrapPrerequisiteQueryDTO&
CanonicalEmptySceneOwner::fixed_query() const noexcept {
  return impl_->fixed_query_;
}

ProjectionBootstrapPrerequisiteStatusDTO
CanonicalEmptySceneOwner::ObserveProjectionBootstrapPrerequisites(
    const ProjectionBootstrapPrerequisiteQueryDTO& query) const {
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
      query,
      ProjectionBootstrapPrerequisiteState::kReadyCommandFenced,
      true,
      true,
      std::nullopt,
      std::nullopt,
  };
  if (!ready.ready_for(query)) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "cannot represent command-fenced bootstrap prerequisites");
  }
  return ready;
}

ProjectionBootstrapAcceptedDTO
CanonicalEmptySceneOwner::BeginProjectionBootstrap(
    const ProjectionBootstrapIntentDTO& intent) {
  ValidateCanonicalEmptyIntent(
      intent, impl_->fixed_query_, impl_->admitted_bundle_);
  std::lock_guard lock(impl_->mutex_);
  auto* database = impl_->database();
  Transaction transaction(database, "BEGIN IMMEDIATE");
  auto state = ReadDurableState(database, impl_->fixed_query_);
  if (state.accepted.has_value()) {
    if (state.accepted->bootstrap_id != intent.bootstrap_id ||
        state.accepted->bootstrap_generation !=
            intent.bootstrap_generation) {
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
  state = ReadDurableState(database, impl_->fixed_query_);
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
    const ProjectionBootstrapRebuildRequestDTO& request) {
  if (!request.valid()) {
    throw std::invalid_argument("bootstrap Rebuild request is invalid");
  }
  std::lock_guard lock(impl_->mutex_);
  auto* database = impl_->database();
  Transaction transaction(database, "BEGIN IMMEDIATE");
  auto state = ReadDurableState(database, impl_->fixed_query_);
  if (!state.accepted.has_value() ||
      request.bootstrap_id != state.accepted->bootstrap_id ||
      request.bootstrap_generation !=
          state.accepted->bootstrap_generation ||
      request.canonical_empty_target != state.accepted->target) {
    throw CanonicalEmptySceneOwnerConflictError(
        "Rebuild does not match the exact durable Begin acceptance");
  }
  if (!state.rebuilt) {
    InsertRebuild(database, *state.accepted);
    state = ReadDurableState(database, impl_->fixed_query_);
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
    const ProjectionBootstrapStatusQueryDTO& query) const {
  if (!query.valid()) {
    throw std::invalid_argument("bootstrap status query is invalid");
  }
  ValidateBoundedIdentity(query.bootstrap_id.value(), "bootstrap_id");
  static_cast<void>(CheckedSafeCounter(
      query.bootstrap_generation.value(), "bootstrap_generation"));
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
    const ProjectionBootstrapFinalizeDTO& request) {
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
    } catch (const std::exception& error) {
      return core::Result<ActiveBundlePublicationReceiptDTO>::Err(
          ErrorDTO{
              "active_bundle_publication_read_uncertain",
              error.what(),
              {},
          });
    } catch (...) {
      return core::Result<ActiveBundlePublicationReceiptDTO>::Err(
          ErrorDTO{
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
    const auto& read_error_code = durable.error().code;
    const bool definitively_not_current =
        read_error_code == "missing_reference" ||
        read_error_code == "stale_generation" ||
        read_error_code == "publication_generation_not_current";
    const bool uncertain = !definitively_not_current;
    return FailedFinalizeResult(
        request, *state.accepted,
        uncertain
            ? ProjectionBootstrapFinalizeResultState::
                  kFailedProjectionUncertain
            : ProjectionBootstrapFinalizeResultState::kStale,
        uncertain ? "active_bundle_publication_read_uncertain"
                  : "active_bundle_publication_not_current",
        uncertain
            ? "the current publication head could not be read "
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

  auto* database = impl_->database();
  Transaction transaction(database, "BEGIN IMMEDIATE");
  state = ReadDurableState(database, impl_->fixed_query_);
  if (!state.accepted.has_value() || !state.rebuilt) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "durable bootstrap state changed before Finalize commit");
  }
  ValidateFinalizeRequest(request, *state.accepted);
  if (!state.finalization_receipt.has_value()) {
    InsertFinalization(database, request);
    state = ReadDurableState(database, impl_->fixed_query_);
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

CanonicalEmptySceneAccessStatus CanonicalEmptySceneOwner::ReadAccessStatus()
    const {
  std::lock_guard lock(impl_->mutex_);
  auto status = AccessStatus(impl_->ReadStateTransaction());
  if (!status.valid()) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "durable state cannot form a valid access-gate status");
  }
  return status;
}

CanonicalEmptySceneStoreIntegrityReport
CanonicalEmptySceneOwner::VerifyIntegrity() const {
  std::lock_guard lock(impl_->mutex_);
  auto* database = impl_->database();
  Transaction transaction(database, "BEGIN");
  ValidateDatabase(
      database, impl_->fixed_query_, impl_->admission_digest_);
  const auto state = ReadDurableState(database, impl_->fixed_query_);
  CanonicalEmptySceneStoreIntegrityReport report{
      impl_->fixed_query_,
      std::string(kCanonicalEmptySceneStoreSchemaId),
      kCanonicalEmptySceneStoreSchemaVersion,
      SchemaArtifactDigest(),
      ScalarInteger(
          database, "SELECT count(*) FROM canonical_empty_scene_acceptance"),
      ScalarInteger(
          database, "SELECT count(*) FROM canonical_empty_scene_rebuild"),
      ScalarInteger(
          database,
          "SELECT count(*) FROM canonical_empty_scene_finalization"),
      AccessStatus(state),
  };
  transaction.Commit();
  if (!report.access.valid() || report.acceptance_row_count < 0 ||
      report.acceptance_row_count > 1 || report.rebuild_row_count < 0 ||
      report.rebuild_row_count > 1 || report.finalization_row_count < 0 ||
      report.finalization_row_count > 1) {
    throw CanonicalEmptySceneOwnerIntegrityError(
        "logical-scene integrity report violates fixed row bounds");
  }
  return report;
}

void CanonicalEmptySceneOwner::Close() {
  std::lock_guard lock(impl_->mutex_);
  if (impl_->database_ == nullptr) {
    return;
  }
  const auto checkpoint = sqlite3_wal_checkpoint_v2(
      impl_->database_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
  if (checkpoint != SQLITE_OK && checkpoint != SQLITE_BUSY &&
      checkpoint != SQLITE_LOCKED) {
    ThrowSqlite(impl_->database_, checkpoint, "checkpoint scene database");
  }
  const auto result = sqlite3_close(impl_->database_);
  if (result != SQLITE_OK) {
    ThrowSqlite(impl_->database_, result, "close scene database");
  }
  impl_->database_ = nullptr;
}

}  // namespace manyforge::scene
