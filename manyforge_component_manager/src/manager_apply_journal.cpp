#include "manyforge_component_manager/manager_apply_journal.hpp"

#include <manyforge_deployment/apply_fence_qualification.hpp>

#include <openssl/evp.h>
#include <sqlite3.h>

#include <manyforge_persistence/sqlite_read_only_preflight.hpp>
#include <manyforge_core/admission_timing_diagnostic.hpp>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>

#include "embedded_manager_apply_journal_schema.hpp"
#include "kernel_apply_fence_rpc_internal.hpp"
#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>
#include "sqlite_store_startup_internal.hpp"

#if SQLITE_VERSION_NUMBER < 3037000
#error "manager Apply journal requires SQLite 3.37.0 or newer"
#endif

#ifndef SQLITE_OPEN_NOFOLLOW
#error "manager Apply journal requires SQLITE_OPEN_NOFOLLOW"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

namespace manyforge::component_manager {
namespace {

static_assert(
    kMaximumManagerApplyFenceComponents ==
    deployment::kMaximumQualifiedApplyFenceControllableComponentsPerTransition);
static_assert(
    kMaximumManagerApplyFenceDomains ==
    deployment::kMaximumQualifiedApplyFenceControlDomainsPerTransition);
static_assert(
    kMaximumManagerApplyFenceBindings ==
    deployment::kMaximumQualifiedApplyFenceControlBindingsPerTransition);
static_assert(kMaximumManagerApplyFenceResources ==
              deployment::kMaximumQualifiedApplyFenceResourcesPerTransition);

using core::ActiveBundlePublicationGeneration;
using core::ActiveBundlePublicationReceiptDTO;
using core::ActiveBundlePublicationRequestDTO;
using core::ActiveControlConfigurationGeneration;
using core::ActiveControlConfigurationId;
using core::ApplyConfigurationFenceIntentDTO;
using core::ApplyFenceBusyDispositionDTO;
using core::ApplyGeneration;
using core::ApplyId;
using core::CellId;
using core::ConfigurationFenceAcceptedDTO;
using core::ConfigurationFenceBindingScopeDTO;
using core::ConfigurationFenceDomainScopeDTO;
using core::ConfigurationFenceGeneration;
using core::ConfigurationFenceId;
using core::ConfigurationFenceReconciliationEvidenceDTO;
using core::ConfigurationFenceReleasedDTO;
using core::ConfigurationFenceReleaseDTO;
using core::ConfigurationFenceScopeDTO;
using core::ContentDigest;
using core::ControlBindingId;
using core::ControlBindingRevision;
using core::ControlDomainId;
using core::ControlDomainRevision;
using core::ControllableComponentId;
using core::DomainActiveControlConfigurationDTO;
using core::GraphRevision;
using core::NamedApplyPublicationTransactionDTO;
using core::ProductContractId;
using core::ProductContractRevision;
using core::ProductId;
using core::ProjectionRevisionDTO;
using core::ProjectionStageGeneration;
using core::PublicRequestContextDTO;
using core::RequestId;
using core::RequestOwnerEpoch;
using core::RequestReplayStatusDTO;
using core::ResourceId;
using core::RetainedBundleId;
using core::RuntimeEpoch;
using core::RuntimeProfileId;
using core::RuntimeProfileRevision;
using core::RuntimeRoleId;
using core::RuntimeUnitId;
using core::SceneId;
using core::SceneRevision;

constexpr std::string_view kDigestDomain =
    "manyforge.component-manager-apply-intent.v2";
constexpr std::string_view kAffectedScopeDigestDomain =
    "manyforge.component-manager-apply-affected-scope.v2";
constexpr std::string_view kFenceAttemptDigestDomain =
    "manyforge.component-manager-apply-fence-attempt.v2";
constexpr std::string_view kFenceAcceptanceDigestDomain =
    "manyforge.component-manager-apply-fence-acceptance.v2";
constexpr std::string_view kRuntimeConvergenceIntentDigestDomain =
    "manyforge.component-manager-runtime-convergence-intent.v2";
constexpr std::string_view kRuntimeStagedReadyDigestDomain =
    "manyforge.component-manager-runtime-staged-ready.v3";
constexpr std::string_view kPrivateMaterializationDigestDomain =
    "manyforge.component-manager-private-selected-bundle.ua4.v2";
constexpr std::string_view kPublicationReceiptDigestDomain =
    "manyforge.component-manager-apply-publication-receipt.v1";
constexpr std::string_view kFenceReleaseIntentDigestDomain =
    "manyforge.component-manager-fence-release-intent.ua4.v2";
constexpr std::string_view kFenceReleasedDigestDomain =
    "manyforge.component-manager-fence-released.v1";
constexpr std::string_view kTerminalOutcomeDigestDomain =
    "manyforge.component-manager-terminal-outcome.ua4.v4";
constexpr int kBusyTimeoutMilliseconds = 10000;
constexpr std::int64_t kWalCheckpointTargetBytes = 16LL * 1024LL * 1024LL;
constexpr std::int64_t kExpectedSchemaObjectCardinality = 27;

[[noreturn]] void ThrowSqlite(sqlite3 *database, const int result,
                              const std::string_view context) {
  const std::string detail =
      database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
  if (result == SQLITE_FULL || result == SQLITE_TOOBIG) {
    throw ManagerApplyJournalLimitError(
        std::string(context) + ": SQLite storage ceiling exhausted: " + detail);
  }
  if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
    throw ManagerApplyJournalBusyError(std::string(context) + ": " + detail);
  }
  if (result == SQLITE_CONSTRAINT) {
    throw ManagerApplyJournalConflictError(std::string(context) + ": " +
                                           detail);
  }
  throw ManagerApplyJournalError(std::string(context) + ": " + detail);
}

void Check(sqlite3 *database, const int result,
           const std::string_view context) {
  if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW) {
    ThrowSqlite(database, result, context);
  }
}

using JournalTimingClock = std::chrono::steady_clock;

std::int64_t JournalElapsedNs(JournalTimingClock::time_point start) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             JournalTimingClock::now() - start).count();
}

const char *JournalStatementStage(std::string_view sql) noexcept {
  if (sql.find("PRAGMA integrity_check") != std::string_view::npos)
    return "manager_apply_sql_integrity_check";
  if (sql.find("PRAGMA foreign_key_check") != std::string_view::npos)
    return "manager_apply_sql_foreign_key_check";
  if (sql.find("SELECT COUNT(*)") != std::string_view::npos)
    return "manager_apply_sql_count";
  if (sql.find("manager_apply_intents") != std::string_view::npos)
    return "manager_apply_sql_intents";
  if (sql.find("manager_apply_affected_scopes") != std::string_view::npos)
    return "manager_apply_sql_scopes";
  if (sql.find("manager_apply_fence_attempts") != std::string_view::npos)
    return "manager_apply_sql_attempts";
  if (sql.find("manager_apply_terminal_outcomes") != std::string_view::npos)
    return "manager_apply_sql_outcomes";
  return "manager_apply_sql_other";
}

void CaptureJournalTiming(const char *stage, JournalTimingClock::time_point start,
                          std::int64_t rows = -1, std::int64_t changes = -1) noexcept {
  core::detail::CaptureAdmissionJournalTiming(stage, JournalElapsedNs(start), rows, changes);
}

class JournalTimingSpan final {
public:
  explicit JournalTimingSpan(const char *stage) noexcept
      : active_(core::detail::CurrentAdmissionTiming() != nullptr), stage_(stage),
        started_(active_ ? JournalTimingClock::now() : JournalTimingClock::time_point{}) {}
  ~JournalTimingSpan() {
    if (active_) core::detail::CaptureAdmissionJournalTiming(
        stage_, JournalElapsedNs(started_), rows_, -1, nullptr,
        core::detail::AdmissionJournalRowsKind::kTableCardinality);
  }
  void Rows(std::int64_t rows) noexcept { rows_ = rows; }
  void Count(const char *stage, std::int64_t count) const noexcept {
    if (active_) core::detail::CaptureAdmissionJournalTiming(
        stage, JournalElapsedNs(started_), count, -1, nullptr,
        core::detail::AdmissionJournalRowsKind::kTableCardinality);
  }
private:
  bool active_;
  const char *stage_;
  JournalTimingClock::time_point started_;
  std::int64_t rows_{-1};
};

void Exec(sqlite3 *database, const std::string_view sql) {
  const bool timing = core::detail::CurrentAdmissionTiming() != nullptr;
  const auto started = timing ? JournalTimingClock::now() : JournalTimingClock::time_point{};
  char *error = nullptr;
  const auto result = sqlite3_exec(database, std::string(sql).c_str(), nullptr,
                                   nullptr, &error);
  if (timing) {
    const char *stage = sql == "COMMIT" ? "manager_apply_sql_commit"
                        : sql == "BEGIN" || sql == "BEGIN IMMEDIATE" ? "manager_apply_sql_begin"
                        : "manager_apply_sql_exec";
    CaptureJournalTiming(stage, started);
  }
  if (result != SQLITE_OK) {
    const std::string detail =
        error == nullptr ? sqlite3_errmsg(database) : error;
    sqlite3_free(error);
    if (result == SQLITE_FULL || result == SQLITE_TOOBIG) {
      throw ManagerApplyJournalLimitError(detail);
    }
    if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
      throw ManagerApplyJournalBusyError(detail);
    }
    if (result == SQLITE_CONSTRAINT) {
      throw ManagerApplyJournalConflictError(detail);
    }
    throw ManagerApplyJournalError(detail);
  }
}

class Statement final {
public:
  Statement(sqlite3 *database, const std::string_view sql)
      : database_(database), timing_(core::detail::CurrentAdmissionTiming() != nullptr),
        stage_(timing_ ? JournalStatementStage(sql) : nullptr) {
    const auto started = timing_ ? JournalTimingClock::now() : JournalTimingClock::time_point{};
    const auto result = sqlite3_prepare_v2(database_, std::string(sql).c_str(), -1,
                                          &statement_, nullptr);
    if (timing_) CaptureJournalTiming("manager_apply_sql_prepare", started);
    Check(database_, result, "prepare SQLite statement");
  }

  ~Statement() {
    if (statement_ != nullptr) {
      if (timing_) core::detail::CaptureAdmissionJournalTiming(
          stage_, step_ns_, rows_returned_, changes_);
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

  void BindBlob(const int index, const std::string_view value) {
    Check(database_,
          sqlite3_bind_blob(statement_, index, value.data(),
                            static_cast<int>(value.size()), SQLITE_TRANSIENT),
          "bind SQLite blob");
  }

  void BindNull(const int index) {
    Check(database_, sqlite3_bind_null(statement_, index), "bind SQLite null");
  }

  [[nodiscard]] bool Step() {
    const auto started = timing_ ? JournalTimingClock::now() : JournalTimingClock::time_point{};
    const auto result = sqlite3_step(statement_);
    if (timing_) {
      step_ns_ += JournalElapsedNs(started);
      if (result == SQLITE_ROW) ++rows_returned_;
      if (result == SQLITE_DONE && !sqlite3_stmt_readonly(statement_))
        changes_ = sqlite3_changes(database_);
    }
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
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-journal text column has an invalid SQLite type");
    }
    const auto *text = sqlite3_column_text(statement_, column);
    if (text == nullptr) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-journal text column is null");
    }
    return {
        reinterpret_cast<const char *>(text),
        static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)),
    };
  }

  [[nodiscard]] std::optional<std::string>
  OptionalText(const int column) const {
    if (sqlite3_column_type(statement_, column) == SQLITE_NULL) {
      return std::nullopt;
    }
    return Text(column);
  }

  [[nodiscard]] std::int64_t Integer(const int column) const {
    if (sqlite3_column_type(statement_, column) != SQLITE_INTEGER) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-journal integer column has an invalid SQLite type");
    }
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] bool IsNull(const int column) const noexcept {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
  }

  [[nodiscard]] std::string Blob(const int column) const {
    if (sqlite3_column_type(statement_, column) != SQLITE_BLOB) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-journal blob column has an invalid SQLite type");
    }
    const auto *bytes = sqlite3_column_blob(statement_, column);
    const auto size = sqlite3_column_bytes(statement_, column);
    if (bytes == nullptr || size <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-journal blob column is empty");
    }
    return {static_cast<const char *>(bytes), static_cast<std::size_t>(size)};
  }

  [[nodiscard]] std::optional<std::string>
  OptionalBlob(const int column) const {
    if (sqlite3_column_type(statement_, column) == SQLITE_NULL) {
      return std::nullopt;
    }
    return Blob(column);
  }

private:
  sqlite3 *database_;
  sqlite3_stmt *statement_{};
  bool timing_{};
  const char *stage_{};
  std::int64_t step_ns_{};
  std::int64_t rows_returned_{};
  std::int64_t changes_{-1};
};

class Transaction final {
public:
  Transaction(sqlite3 *database, const std::string_view begin)
      : database_(database), timing_(core::detail::CurrentAdmissionTiming() != nullptr),
        started_(timing_ ? JournalTimingClock::now() : JournalTimingClock::time_point{}) {
    Exec(database_, begin);
  }

  ~Transaction() {
    if (!finished_) {
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    if (timing_) CaptureJournalTiming(
        finished_ ? "manager_apply_transaction_commit" : "manager_apply_transaction_rollback",
        started_, -1, -1);
  }

  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

  void Commit() {
    Exec(database_, "COMMIT");
    finished_ = true;
  }

private:
  sqlite3 *database_;
  bool timing_{};
  JournalTimingClock::time_point started_;
  bool finished_{};
};

std::int64_t ScalarInteger(sqlite3 *database, const std::string_view sql) {
  Statement statement(database, sql);
  if (!statement.Step()) {
    throw ManagerApplyJournalIntegrityError("SQLite scalar row is missing");
  }
  const auto result = statement.Integer(0);
  if (statement.Step()) {
    throw ManagerApplyJournalIntegrityError(
        "SQLite scalar query returned multiple rows");
  }
  return result;
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
                "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name LIMIT ?");
  statement.Bind(1, kExpectedSchemaObjectCardinality + 1);
  SchemaObjects objects;
  std::int64_t count = 0;
  while (statement.Step()) {
    ++count;
    if (count > kExpectedSchemaObjectCardinality) {
      throw ManagerApplyJournalIntegrityError(
          "Apply-journal schema object cardinality exceeds the "
          "versioned artifact");
    }
    objects.emplace(std::make_pair(statement.Text(0), statement.Text(1)),
                    NormalizedSql(statement.Text(2)));
  }
  return objects;
}

const SchemaObjects &ExpectedSchemaObjects() {
  static const SchemaObjects expected = [] {
    sqlite3 *database = nullptr;
    const auto result =
        sqlite3_open_v2(":memory:", &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (result != SQLITE_OK) {
      const std::string detail = database == nullptr ? sqlite3_errstr(result)
                                                     : sqlite3_errmsg(database);
      if (database != nullptr) {
        sqlite3_close(database);
      }
      throw ManagerApplyJournalError("open in-memory Apply schema validator: " +
                                     detail);
    }
    try {
      Exec(database, detail::kEmbeddedManagerApplyJournalSchema);
      auto objects = ReadSchemaObjects(database);
      if (objects.size() !=
          static_cast<std::size_t>(kExpectedSchemaObjectCardinality)) {
        throw ManagerApplyJournalError(
            "embedded Apply schema object cardinality is invalid");
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

void ValidateBoundedIdentity(const std::string_view value,
                             const std::string_view label) {
  if (value.empty() ||
      value.size() > kMaximumManagerApplyJournalIdentifierBytes ||
      std::any_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x21U || byte > 0x7EU;
      })) {
    throw std::invalid_argument(
        std::string(label) +
        " must be a bounded non-empty printable ASCII identity");
  }
}

void ValidateBoundedText(const std::string_view value,
                         const std::size_t maximum_bytes,
                         const std::string_view label) {
  if (value.empty() || value.size() > maximum_bytes ||
      std::any_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20U || byte == 0x7FU;
      })) {
    throw std::invalid_argument(
        std::string(label) +
        " must be bounded, non-empty, and contain no control bytes");
  }
}

bool IsSha256Digest(const std::string_view value) {
  return value.size() == 71U && value.starts_with("sha256:") &&
         std::all_of(value.begin() + 7, value.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

void ValidateDigest(const std::string_view value,
                    const std::string_view label) {
  if (!IsSha256Digest(value)) {
    throw std::invalid_argument(std::string(label) +
                                " must be a lowercase sha256 digest");
  }
}

std::int64_t CheckedSigned(const std::uint64_t value,
                           const std::string_view label) {
  if (value == 0U || value > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument(
        std::string(label) +
        " must fit the positive signed SQLite int64 range");
  }
  return static_cast<std::int64_t>(value);
}

std::string Hex(const std::span<const unsigned char> bytes) {
  constexpr std::array<char, 16> kHex{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const auto byte : bytes) {
    result.push_back(kHex[byte >> 4U]);
    result.push_back(kHex[byte & 0x0FU]);
  }
  return result;
}

std::string RawDigest(const std::span<const std::byte> bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
  unsigned int output_length = 0;
  auto *context = EVP_MD_CTX_new();
  if (context == nullptr) {
    throw ManagerApplyJournalIntegrityError("cannot allocate SHA-256 context");
  }
  const auto release = [&]() { EVP_MD_CTX_free(context); };
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, bytes.data(), bytes.size()) != 1 ||
      EVP_DigestFinal_ex(context, output.data(), &output_length) != 1) {
    release();
    throw ManagerApplyJournalIntegrityError("SHA-256 computation failed");
  }
  release();
  return "sha256:" +
         Hex(std::span<const unsigned char>(output.data(), output_length));
}

std::string RawDigest(const std::string_view value) {
  return RawDigest(std::as_bytes(std::span(value.data(), value.size())));
}

void AppendField(std::string &record, const std::string_view name,
                 const std::string_view value) {
  record.append(std::to_string(name.size()));
  record.push_back(':');
  record.append(name);
  record.push_back('=');
  record.append(std::to_string(value.size()));
  record.push_back(':');
  record.append(value);
  record.push_back('\n');
}

/// The bounded cause details of a retained failure, length-prefixed in key
/// order like the digest records (`AppendField`); empty for no details.
[[nodiscard]] std::string EncodeCauseDetails(
    const std::map<std::string, std::string> &details) {
  std::string encoded;
  for (const auto &[key, value] : details) {
    AppendField(encoded, key, value);
  }
  return encoded;
}

[[nodiscard]] std::map<std::string, std::string>
DecodeCauseDetails(const std::string_view encoded) {
  const auto length = [&](std::size_t &offset, const char separator) {
    const auto end = encoded.find(separator, offset);
    if (end == std::string_view::npos || end == offset || end - offset > 6U) {
      throw ManagerApplyJournalIntegrityError(
          "stored terminal Apply cause details are malformed");
    }
    std::size_t value = 0U;
    for (auto index = offset; index < end; ++index) {
      const auto digit = encoded[index];
      if (digit < '0' || digit > '9') {
        throw ManagerApplyJournalIntegrityError(
            "stored terminal Apply cause details are malformed");
      }
      value = value * 10U + static_cast<std::size_t>(digit - '0');
    }
    offset = end + 1U;
    return value;
  };
  const auto text = [&](std::size_t &offset, const std::size_t size,
                        const char terminator) {
    if (offset + size >= encoded.size() || encoded[offset + size] != terminator) {
      throw ManagerApplyJournalIntegrityError(
          "stored terminal Apply cause details are malformed");
    }
    std::string value(encoded.substr(offset, size));
    offset += size + 1U;
    return value;
  };
  std::map<std::string, std::string> details;
  std::size_t offset = 0U;
  while (offset < encoded.size()) {
    auto key = text(offset, length(offset, ':'), '=');
    auto value = text(offset, length(offset, ':'), '\n');
    if (!details.emplace(std::move(key), std::move(value)).second) {
      throw ManagerApplyJournalIntegrityError(
          "stored terminal Apply cause details repeat a key");
    }
  }
  return details;
}

struct EncodedIntent final {
  std::int64_t apply_generation;
  std::int64_t predecessor_scene_revision_id;
  std::optional<std::int64_t> expected_admission_generation;
  std::optional<std::string> parent_authoring_claim;
  std::string intent_digest;
};

std::string EncodeParentAuthoringClaim(
    const core::CellAdmissionClaimDTO &claim) {
  return detail::EncodeKernelCellAdmissionRpcResponse(
      "cell_admission_acquire", claim);
}

core::CellAdmissionClaimDTO DecodeParentAuthoringClaim(
    const std::string_view bytes) {
  const auto packet = detail::DecodeKernelCellAdmissionRpcResponse(bytes);
  const auto *claim =
      std::get_if<core::CellAdmissionClaimDTO>(&packet.response);
  if (packet.command != "cell_admission_acquire" || claim == nullptr ||
      !core::ValidateCellAdmissionClaimIntent(claim->intent).ok()) {
    throw ManagerApplyJournalIntegrityError(
        "stored parent authoring claim is invalid");
  }
  return *claim;
}

EncodedIntent EncodeIntent(const ManagerApplyIntentDTO &intent) {
  if (!intent.valid()) {
    throw std::invalid_argument("manager Apply intent is invalid");
  }
  ValidateBoundedIdentity(intent.request_context.request_id.value(),
                          "request_id");
  ValidateBoundedIdentity(intent.request_context.request_owner_epoch.value(),
                          "request_owner_epoch");
  ValidateBoundedText(intent.request_context.source_label,
                      kMaximumManagerApplySourceLabelBytes, "source_label");
  if (intent.request_context.correlation_id.has_value()) {
    ValidateBoundedText(*intent.request_context.correlation_id,
                        kMaximumManagerApplyCorrelationIdBytes,
                        "correlation_id");
  }
  ValidateBoundedIdentity(intent.apply_id.value(), "apply_id");
  ValidateBoundedIdentity(intent.cell_id.value(), "cell_id");
  ValidateBoundedIdentity(
      intent.expected_active_projection_revision.retained_bundle_id.value(),
      "predecessor_retained_bundle_id");
  ValidateBoundedIdentity(
      intent.expected_active_projection_revision.graph_revision.value(),
      "predecessor_graph_revision");
  ValidateBoundedIdentity(intent.expected_active_projection_revision
                              .scene_revision.scene_id.value(),
                          "predecessor_scene_id");
  ValidateDigest(intent.expected_active_projection_revision.scene_revision
                     .content_hash.value(),
                 "predecessor_scene_content_hash");
  ValidateBoundedIdentity(intent.candidate_retained_bundle_id.value(),
                          "candidate_retained_bundle_id");
  ValidateBoundedIdentity(intent.candidate_graph_revision.value(),
                          "candidate_graph_revision");
  if (!intent.prepared_activation.valid()) {
    throw std::invalid_argument(
        "manager Apply intent has no exact prepared activation");
  }
  ValidateDigest(intent.prepared_activation.captured_input_digest.value(),
                 "captured_input_digest");
  ValidateBoundedIdentity(
      intent.prepared_activation.deployment_artifact_id.value(),
      "deployment_artifact_id");
  ValidateDigest(intent.prepared_activation.deployment_revision.value(),
                 "deployment_revision");
  if (intent.prepared_activation.program_binding_id.has_value()) {
    ValidateDigest(intent.prepared_activation.program_binding_id->value(),
                   "program_binding_id");
  }
  ValidateDigest(intent.prepared_activation.prepared_activation_id.value(),
                 "prepared_activation_id");
  ValidateBoundedText(intent.prepared_activation.canonical_receipt_json,
                      kMaximumManagerApplyPreparedReceiptBytes,
                      "prepared_activation_receipt_json");

  const auto generation =
      CheckedSigned(intent.apply_generation.value(), "apply_generation");
  const auto scene_revision = CheckedSigned(
      intent.expected_active_projection_revision.scene_revision.revision_id,
      "predecessor_scene_revision_id");
  const auto expected_admission_generation =
      intent.expected_admission_generation.has_value()
          ? std::optional<std::int64_t>(CheckedSigned(
                intent.expected_admission_generation->value(),
                "expected_admission_generation"))
          : std::nullopt;
  const auto parent_authoring_claim =
      intent.parent_authoring_claim.has_value()
          ? std::optional<std::string>(
                EncodeParentAuthoringClaim(*intent.parent_authoring_claim))
          : std::nullopt;

  std::string record;
  AppendField(record, "domain", kDigestDomain);
  AppendField(record, "request_id", intent.request_context.request_id.value());
  AppendField(record, "request_owner_epoch",
              intent.request_context.request_owner_epoch.value());
  AppendField(record, "source_label", intent.request_context.source_label);
  AppendField(record, "correlation_id_present",
              intent.request_context.correlation_id.has_value() ? "1" : "0");
  AppendField(record, "correlation_id",
              intent.request_context.correlation_id.value_or(""));
  AppendField(record, "scene_initialization",
              ToString(intent.scene_initialization));
  AppendField(record, "apply_id", intent.apply_id.value());
  AppendField(record, "apply_generation", std::to_string(generation));
  AppendField(record, "cell_id", intent.cell_id.value());
  AppendField(
      record, "predecessor_retained_bundle_id",
      intent.expected_active_projection_revision.retained_bundle_id.value());
  AppendField(
      record, "predecessor_graph_revision",
      intent.expected_active_projection_revision.graph_revision.value());
  AppendField(record, "predecessor_scene_id",
              intent.expected_active_projection_revision.scene_revision.scene_id
                  .value());
  AppendField(record, "predecessor_scene_revision_id",
              std::to_string(scene_revision));
  AppendField(record, "predecessor_scene_content_hash",
              intent.expected_active_projection_revision.scene_revision
                  .content_hash.value());
  AppendField(record, "candidate_retained_bundle_id",
              intent.candidate_retained_bundle_id.value());
  AppendField(record, "candidate_graph_revision",
              intent.candidate_graph_revision.value());
  AppendField(record, "captured_input_digest",
              intent.prepared_activation.captured_input_digest.value());
  AppendField(record, "deployment_artifact_id",
              intent.prepared_activation.deployment_artifact_id.value());
  AppendField(record, "deployment_revision",
              intent.prepared_activation.deployment_revision.value());
  // Empty only for a non-executable Program; a bound id is a digest.
  AppendField(record, "program_binding_id",
              intent.prepared_activation.program_binding_id.has_value()
                  ? intent.prepared_activation.program_binding_id->value()
                  : std::string{});
  AppendField(record, "prepared_activation_id",
              intent.prepared_activation.prepared_activation_id.value());
  AppendField(record, "prepared_activation_receipt_json",
              intent.prepared_activation.canonical_receipt_json);
  AppendField(record, "admission_context_kind",
              expected_admission_generation.has_value()
                  ? "direct"
                  : "parent_authoring_claim");
  AppendField(record, "expected_admission_generation",
              expected_admission_generation.has_value()
                  ? std::to_string(*expected_admission_generation)
                  : "");
  AppendField(record, "parent_authoring_claim",
              parent_authoring_claim.value_or(""));
  return {generation, scene_revision, expected_admission_generation,
          parent_authoring_claim, RawDigest(record)};
}

std::int64_t CheckedNonnegativeSigned(const std::uint64_t value,
                                      const std::string_view label) {
  if (value >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument(std::string(label) +
                                " must fit the nonnegative SQLite int64 range");
  }
  return static_cast<std::int64_t>(value);
}

std::string EncodeTokens(const std::vector<std::string> &tokens) {
  std::string result = std::to_string(tokens.size()) + ";";
  for (const auto &token : tokens) {
    result += std::to_string(token.size());
    result.push_back(':');
    result += token;
  }
  return result;
}

std::uint64_t ParseCanonicalUnsigned(const std::string_view value,
                                     const std::string_view label) {
  if (value.empty() || (value.size() > 1U && value.front() == '0')) {
    throw ManagerApplyJournalIntegrityError(
        std::string("stored ") + std::string(label) +
        " has a noncanonical integer encoding");
  }
  std::uint64_t result = 0U;
  for (const char character : value) {
    if (character < '0' || character > '9' ||
        result > (std::numeric_limits<std::uint64_t>::max() -
                  static_cast<std::uint64_t>(character - '0')) /
                     10U) {
      throw ManagerApplyJournalIntegrityError(
          std::string("stored ") + std::string(label) +
          " has an invalid integer encoding");
    }
    result = result * 10U + static_cast<std::uint64_t>(character - '0');
  }
  return result;
}

std::vector<std::string> DecodeTokens(const std::string_view encoding,
                                      const std::size_t maximum_tokens,
                                      const std::string_view label) {
  const auto separator = encoding.find(';');
  if (separator == std::string_view::npos) {
    throw ManagerApplyJournalIntegrityError(
        std::string("stored ") + std::string(label) + " encoding is invalid");
  }
  const auto count_value =
      ParseCanonicalUnsigned(encoding.substr(0, separator), label);
  if (count_value > maximum_tokens) {
    throw ManagerApplyJournalIntegrityError(std::string("stored ") +
                                            std::string(label) +
                                            " cardinality exceeds its ceiling");
  }
  std::vector<std::string> result;
  result.reserve(static_cast<std::size_t>(count_value));
  std::size_t cursor = separator + 1U;
  for (std::uint64_t index = 0U; index < count_value; ++index) {
    const auto colon = encoding.find(':', cursor);
    if (colon == std::string_view::npos) {
      throw ManagerApplyJournalIntegrityError(
          std::string("stored ") + std::string(label) + " token is invalid");
    }
    const auto size_value =
        ParseCanonicalUnsigned(encoding.substr(cursor, colon - cursor), label);
    if (size_value > encoding.size() - colon - 1U) {
      throw ManagerApplyJournalIntegrityError(std::string("stored ") +
                                              std::string(label) +
                                              " token exceeds its encoding");
    }
    const auto size = static_cast<std::size_t>(size_value);
    result.emplace_back(encoding.substr(colon + 1U, size));
    cursor = colon + 1U + size;
  }
  if (cursor != encoding.size() || EncodeTokens(result) != encoding) {
    throw ManagerApplyJournalIntegrityError(std::string("stored ") +
                                            std::string(label) +
                                            " encoding is noncanonical");
  }
  return result;
}

std::string
EncodeComponents(const std::vector<ControllableComponentId> &components) {
  std::vector<std::string> tokens;
  tokens.reserve(components.size());
  for (const auto &component : components) {
    ValidateBoundedIdentity(component.value(), "controllable_component_id");
    tokens.push_back(component.value());
  }
  return EncodeTokens(tokens);
}

template <typename Revision>
void AppendOptionalRevision(std::vector<std::string> &tokens,
                            const std::optional<Revision> &revision,
                            const std::string_view label) {
  tokens.emplace_back(revision.has_value() ? "1" : "0");
  if (revision.has_value()) {
    ValidateBoundedIdentity(revision->value(), label);
    tokens.push_back(revision->value());
  } else {
    tokens.emplace_back();
  }
}

std::string EncodeDomains(const ConfigurationFenceScopeDTO &scope) {
  if (std::any_of(
          scope.domains.begin(), scope.domains.end(), [](const auto &domain) {
            return std::holds_alternative<
                core::ProviderConfigurationFenceDomainDTO>(domain.payload);
          }))
    return "MF-PROVIDER-FENCE-DOMAINS-V1:" +
           detail::EncodeConfigurationFenceScopeRecord(scope);
  const auto &domains = scope.domains;
  std::vector<std::string> tokens;
  tokens.reserve(domains.size() * 5U);
  for (const auto &domain : domains) {
    ValidateBoundedIdentity(domain.control_domain_id.value(),
                            "control_domain_id");
    tokens.push_back(domain.control_domain_id.value());
    AppendOptionalRevision(tokens, domain.predecessor_control_domain_revision,
                           "predecessor_control_domain_revision");
    AppendOptionalRevision(tokens, domain.candidate_control_domain_revision,
                           "candidate_control_domain_revision");
  }
  return EncodeTokens(tokens);
}

std::string
EncodeBindings(const std::vector<ConfigurationFenceBindingScopeDTO> &bindings) {
  std::vector<std::string> tokens;
  tokens.reserve(bindings.size() * 5U);
  for (const auto &binding : bindings) {
    ValidateBoundedIdentity(binding.control_binding_id.value(),
                            "control_binding_id");
    tokens.push_back(binding.control_binding_id.value());
    AppendOptionalRevision(tokens, binding.predecessor_control_binding_revision,
                           "predecessor_control_binding_revision");
    AppendOptionalRevision(tokens, binding.candidate_control_binding_revision,
                           "candidate_control_binding_revision");
  }
  return EncodeTokens(tokens);
}

std::string EncodeResources(const std::vector<ResourceId> &resources) {
  std::vector<std::string> tokens;
  tokens.reserve(resources.size());
  for (const auto &resource : resources) {
    ValidateBoundedIdentity(resource.value(), "resource_id");
    tokens.push_back(resource.value());
  }
  return EncodeTokens(tokens);
}

std::string EncodeActiveConfigurations(
    const std::vector<DomainActiveControlConfigurationDTO> &configurations) {
  std::vector<std::string> tokens;
  tokens.reserve(configurations.size() * 3U);
  for (const auto &configuration : configurations) {
    ValidateBoundedIdentity(configuration.control_domain_id.value(),
                            "active_configuration_domain_id");
    ValidateBoundedIdentity(
        configuration.active_control_configuration_id.value(),
        "active_control_configuration_id");
    tokens.push_back(configuration.control_domain_id.value());
    tokens.push_back(configuration.active_control_configuration_id.value());
    tokens.push_back(std::to_string(CheckedSigned(
        configuration.active_control_configuration_generation.value(),
        "active_control_configuration_generation")));
  }
  return EncodeTokens(tokens);
}

template <typename Revision>
[[nodiscard]] std::optional<Revision> DecodeOptionalRevision(
    const std::vector<std::string> &tokens, const std::size_t presence_index,
    const std::size_t revision_index, const std::string_view label) {
  if (tokens[presence_index] == "0") {
    if (!tokens[revision_index].empty()) {
      throw ManagerApplyJournalIntegrityError(std::string("stored ") +
                                              std::string(label) +
                                              " absent revision has bytes");
    }
    return std::nullopt;
  }
  if (tokens[presence_index] != "1" || tokens[revision_index].empty()) {
    throw ManagerApplyJournalIntegrityError(std::string("stored ") +
                                            std::string(label) +
                                            " revision presence is invalid");
  }
  return Revision(tokens[revision_index]);
}

ConfigurationFenceScopeDTO DecodeScope(const std::string &components_encoding,
                                       const std::string &domains_encoding,
                                       const std::string &bindings_encoding,
                                       const std::string &resources_encoding) {
  constexpr std::string_view provider_prefix = "MF-PROVIDER-FENCE-DOMAINS-V1:";
  if (domains_encoding.starts_with(provider_prefix)) {
    auto scope = detail::DecodeConfigurationFenceScopeRecord(
        std::string_view(domains_encoding).substr(provider_prefix.size()));
    if (scope.controllable_component_ids.size() >
            kMaximumManagerApplyFenceComponents ||
        scope.domains.size() > kMaximumManagerApplyFenceDomains ||
        scope.bindings.size() > kMaximumManagerApplyFenceBindings ||
        scope.resources.size() > kMaximumManagerApplyFenceResources ||
        EncodeDomains(scope) != domains_encoding ||
        EncodeComponents(scope.controllable_component_ids) !=
            components_encoding ||
        EncodeBindings(scope.bindings) != bindings_encoding ||
        EncodeResources(scope.resources) != resources_encoding)
      throw ManagerApplyJournalIntegrityError(
          "stored provider fence scope is inconsistent");
    return scope;
  }
  const auto component_tokens = DecodeTokens(
      components_encoding, kMaximumManagerApplyFenceComponents, "components");
  const auto domain_tokens = DecodeTokens(
      domains_encoding, kMaximumManagerApplyFenceDomains * 5U, "domains");
  const auto binding_tokens = DecodeTokens(
      bindings_encoding, kMaximumManagerApplyFenceBindings * 5U, "bindings");
  const auto resource_tokens = DecodeTokens(
      resources_encoding, kMaximumManagerApplyFenceResources, "resources");
  if (domain_tokens.size() % 5U != 0U || binding_tokens.size() % 5U != 0U) {
    throw ManagerApplyJournalIntegrityError(
        "stored fence scope tuple cardinality is invalid");
  }
  std::vector<ControllableComponentId> decoded_components;
  std::vector<ConfigurationFenceDomainScopeDTO> decoded_domains;
  std::vector<ConfigurationFenceBindingScopeDTO> decoded_bindings;
  std::vector<ResourceId> decoded_resources;
  for (const auto &component : component_tokens) {
    decoded_components.emplace_back(component);
  }
  for (std::size_t index = 0U; index < domain_tokens.size(); index += 5U) {
    decoded_domains.push_back({
        ControlDomainId(domain_tokens[index]),
        DecodeOptionalRevision<ControlDomainRevision>(
            domain_tokens, index + 1U, index + 2U,
            "predecessor control-domain"),
        DecodeOptionalRevision<ControlDomainRevision>(
            domain_tokens, index + 3U, index + 4U, "candidate control-domain"),
    });
  }
  for (std::size_t index = 0U; index < binding_tokens.size(); index += 5U) {
    decoded_bindings.push_back({
        ControlBindingId(binding_tokens[index]),
        DecodeOptionalRevision<ControlBindingRevision>(
            binding_tokens, index + 1U, index + 2U,
            "predecessor control-binding"),
        DecodeOptionalRevision<ControlBindingRevision>(
            binding_tokens, index + 3U, index + 4U,
            "candidate control-binding"),
    });
  }
  for (const auto &resource : resource_tokens) {
    decoded_resources.emplace_back(resource);
  }
  ConfigurationFenceScopeDTO scope{
      std::move(decoded_components),
      std::move(decoded_domains),
      std::move(decoded_bindings),
      std::move(decoded_resources),
  };
  if (!scope.valid()) {
    throw ManagerApplyJournalIntegrityError("stored fence scope is invalid");
  }
  return scope;
}

ApplyFenceBusyDispositionDTO DecodeBusyDisposition(const std::string &value) {
  if (value != "reject_if_affected_busy") {
    throw ManagerApplyJournalIntegrityError(
        "stored Apply-fence busy disposition is invalid");
  }
  return ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy;
}

std::vector<DomainActiveControlConfigurationDTO>
DecodeActiveConfigurations(const std::string &encoding) {
  const auto tokens = DecodeTokens(
      encoding, kMaximumManagerApplyFenceDomains * 3U, "active configurations");
  if (tokens.size() % 3U != 0U) {
    throw ManagerApplyJournalIntegrityError(
        "stored active-configuration tuple cardinality is invalid");
  }
  std::vector<DomainActiveControlConfigurationDTO> result;
  for (std::size_t index = 0U; index < tokens.size(); index += 3U) {
    const auto generation =
        ParseCanonicalUnsigned(tokens[index + 2U], "active generation");
    if (generation == 0U ||
        generation > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())) {
      throw ManagerApplyJournalIntegrityError(
          "stored active-configuration generation is invalid");
    }
    result.push_back({
        ControlDomainId(tokens[index]),
        ActiveControlConfigurationId(tokens[index + 1U]),
        ActiveControlConfigurationGeneration(generation),
    });
  }
  return result;
}

std::uint64_t ParseCanonicalSqliteUnsigned(const std::string_view value,
                                           const std::string_view label,
                                           const bool require_nonzero) {
  const auto result = ParseCanonicalUnsigned(value, label);
  if ((require_nonzero && result == 0U) ||
      result > static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max())) {
    throw ManagerApplyJournalIntegrityError(
        std::string("stored ") + std::string(label) +
        " is outside the signed SQLite int64 range");
  }
  return result;
}

RequestReplayStatusDTO
DecodeAcceptedReplayStatus(const std::string_view value) {
  if (value == "accepted_new") {
    return RequestReplayStatusDTO::kAcceptedNew;
  }
  if (value == "replayed_exact") {
    return RequestReplayStatusDTO::kReplayedExact;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored accepted replay status is invalid");
}

std::string EncodeFenceReconciliationEvidence(
    const ConfigurationFenceReconciliationEvidenceDTO &evidence) {
  if (!evidence.valid()) {
    throw std::invalid_argument(
        "configuration-fence reconciliation evidence is invalid");
  }
  if (evidence.reconciled_scope.controllable_component_ids.size() >
          kMaximumManagerApplyFenceComponents ||
      evidence.reconciled_scope.domains.size() >
          kMaximumManagerApplyFenceDomains ||
      evidence.reconciled_scope.bindings.size() >
          kMaximumManagerApplyFenceBindings ||
      evidence.reconciled_scope.resources.size() >
          kMaximumManagerApplyFenceResources) {
    throw ManagerApplyJournalLimitError(
        "configuration-fence reconciliation scope exceeds a fixed limit");
  }
  ValidateBoundedIdentity(evidence.execution_host_epoch.runtime_unit_id.value(),
                          "Kernel runtime_unit_id");
  ValidateBoundedIdentity(evidence.execution_host_epoch.runtime_epoch.value(),
                          "Kernel runtime_epoch");
  ValidateBoundedIdentity(
      evidence.execution_host_epoch.request_owner_epoch.value(),
      "Kernel request_owner_epoch");
  ValidateDigest(
      evidence.execution_host_epoch.durable_stop_fence_ledger_digest.value(),
      "durable_stop_fence_ledger_digest");
  ValidateBoundedIdentity(evidence.owner_transaction.apply_id.value(),
                          "apply_id");
  ValidateBoundedIdentity(evidence.configuration_fence_id.value(),
                          "configuration_fence_id");
  ValidateBoundedIdentity(evidence.active_graph_revision.value(),
                          "active_graph_revision");
  ValidateBoundedIdentity(
      evidence.active_projection_revision.retained_bundle_id.value(),
      "active_retained_bundle_id");
  ValidateBoundedIdentity(
      evidence.active_projection_revision.graph_revision.value(),
      "active_projection_graph_revision");
  ValidateBoundedIdentity(
      evidence.active_projection_revision.scene_revision.scene_id.value(),
      "active_scene_id");
  ValidateDigest(
      evidence.active_projection_revision.scene_revision.content_hash.value(),
      "active_scene_content_hash");
  ValidateDigest(evidence.active_public_pointer_digest.value(),
                 "active_public_pointer_digest");
  ValidateDigest(evidence.evidence_digest.value(), "evidence_digest");

  const auto host_publication_generation = CheckedSigned(
      evidence.execution_host_epoch.publication_generation.value(),
      "Kernel publication_generation");
  const auto apply_generation = CheckedSigned(
      evidence.owner_transaction.apply_generation.value(), "apply_generation");
  const auto fence_generation =
      CheckedSigned(evidence.configuration_fence_generation.value(),
                    "configuration_fence_generation");
  const auto operation_generation = CheckedNonnegativeSigned(
      evidence.operation_resource_generation, "operation_resource_generation");
  const auto scene_revision = CheckedSigned(
      evidence.active_projection_revision.scene_revision.revision_id,
      "active_scene_revision_id");
  const auto active_publication_generation =
      CheckedSigned(evidence.active_publication_generation.value(),
                    "active_publication_generation");
  const auto queued_count =
      CheckedNonnegativeSigned(evidence.affected_queued_operation_count,
                               "affected_queued_operation_count");
  const auto active_count =
      CheckedNonnegativeSigned(evidence.affected_active_operation_count,
                               "affected_active_operation_count");

  return EncodeTokens({
      evidence.execution_host_epoch.runtime_unit_id.value(),
      evidence.execution_host_epoch.runtime_epoch.value(),
      evidence.execution_host_epoch.request_owner_epoch.value(),
      evidence.execution_host_epoch.durable_stop_fence_ledger_digest.value(),
      std::to_string(host_publication_generation),
      evidence.owner_transaction.apply_id.value(),
      std::to_string(apply_generation),
      evidence.configuration_fence_id.value(),
      std::to_string(fence_generation),
      EncodeComponents(evidence.reconciled_scope.controllable_component_ids),
      EncodeDomains(evidence.reconciled_scope),
      EncodeBindings(evidence.reconciled_scope.bindings),
      EncodeResources(evidence.reconciled_scope.resources),
      std::to_string(operation_generation),
      evidence.active_graph_revision.value(),
      evidence.active_projection_revision.retained_bundle_id.value(),
      evidence.active_projection_revision.graph_revision.value(),
      evidence.active_projection_revision.scene_revision.scene_id.value(),
      std::to_string(scene_revision),
      evidence.active_projection_revision.scene_revision.content_hash.value(),
      std::to_string(active_publication_generation),
      evidence.active_public_pointer_digest.value(),
      EncodeActiveConfigurations(evidence.active_control_configurations),
      std::to_string(queued_count),
      std::to_string(active_count),
      evidence.evidence_digest.value(),
  });
}

ConfigurationFenceReconciliationEvidenceDTO
DecodeFenceReconciliationEvidence(const std::string &encoding) {
  const auto tokens = DecodeTokens(encoding, 26U, "fence reconciliation");
  if (tokens.size() != 26U) {
    throw ManagerApplyJournalIntegrityError(
        "stored fence reconciliation tuple cardinality is invalid");
  }
  ConfigurationFenceReconciliationEvidenceDTO evidence{
      core::ExecutionHostEpochDTO{
          RuntimeUnitId(tokens[0]),
          RuntimeEpoch(tokens[1]),
          RequestOwnerEpoch(tokens[2]),
          ContentDigest(tokens[3]),
          ActiveBundlePublicationGeneration(ParseCanonicalSqliteUnsigned(
              tokens[4], "Kernel publication generation", true)),
      },
      core::ApplyFenceOwnerDTO{
          ApplyId(tokens[5]),
          ApplyGeneration(ParseCanonicalSqliteUnsigned(
              tokens[6], "release Apply generation", true)),
      },
      ConfigurationFenceId(tokens[7]),
      ConfigurationFenceGeneration(ParseCanonicalSqliteUnsigned(
          tokens[8], "release fence generation", true)),
      DecodeScope(tokens[9], tokens[10], tokens[11], tokens[12]),
      ParseCanonicalSqliteUnsigned(tokens[13], "operation generation", false),
      GraphRevision(tokens[14]),
      ProjectionRevisionDTO{
          RetainedBundleId(tokens[15]),
          GraphRevision(tokens[16]),
          SceneRevision{
              SceneId(tokens[17]),
              ParseCanonicalSqliteUnsigned(tokens[18], "active scene revision",
                                           true),
              ContentDigest(tokens[19]),
          },
      },
      ActiveBundlePublicationGeneration(ParseCanonicalSqliteUnsigned(
          tokens[20], "active publication generation", true)),
      ContentDigest(tokens[21]),
      DecodeActiveConfigurations(tokens[22]),
      ParseCanonicalSqliteUnsigned(tokens[23], "affected queued count", false),
      ParseCanonicalSqliteUnsigned(tokens[24], "affected active count", false),
      ContentDigest(tokens[25]),
  };
  if (!evidence.valid() ||
      EncodeFenceReconciliationEvidence(evidence) != encoding) {
    throw ManagerApplyJournalIntegrityError(
        "stored fence reconciliation evidence is invalid or noncanonical");
  }
  return evidence;
}

std::string
EncodeFenceReleaseRequest(const ConfigurationFenceReleaseDTO &request) {
  if (!request.valid()) {
    throw std::invalid_argument(
        "configuration-fence release request is invalid");
  }
  ValidateBoundedIdentity(request.manager_runtime_epoch.value(),
                          "manager_runtime_epoch");
  return EncodeTokens(
      {EncodeFenceReconciliationEvidence(request.reconciliation_evidence),
       request.manager_runtime_epoch.value()});
}

ConfigurationFenceReleaseDTO
DecodeFenceReleaseRequest(const std::string &encoding) {
  const auto tokens = DecodeTokens(encoding, 2U, "fence release request");
  if (tokens.size() != 2U) {
    throw ManagerApplyJournalIntegrityError(
        "stored fence release-request tuple cardinality is invalid");
  }
  ConfigurationFenceReleaseDTO request{
      DecodeFenceReconciliationEvidence(tokens[0]), RuntimeEpoch(tokens[1])};
  if (!request.valid() || EncodeFenceReleaseRequest(request) != encoding) {
    throw ManagerApplyJournalIntegrityError(
        "stored fence release request is invalid or noncanonical");
  }
  return request;
}

std::string EncodeFenceReleased(const ConfigurationFenceReleasedDTO &released) {
  if (!released.valid()) {
    throw std::invalid_argument(
        "configuration-fence release receipt is invalid");
  }
  ValidateDigest(released.release_receipt_digest.value(),
                 "release_receipt_digest");
  const auto host_publication_generation = CheckedSigned(
      released.execution_host_epoch.publication_generation.value(),
      "released Kernel publication_generation");
  return EncodeTokens({
      EncodeFenceReleaseRequest(released.request),
      released.execution_host_epoch.runtime_unit_id.value(),
      released.execution_host_epoch.runtime_epoch.value(),
      released.execution_host_epoch.request_owner_epoch.value(),
      released.execution_host_epoch.durable_stop_fence_ledger_digest.value(),
      std::to_string(host_publication_generation),
      std::to_string(released.released_at_ns),
      std::string(core::to_string(released.replay_status)),
      released.release_receipt_digest.value(),
  });
}

ConfigurationFenceReleasedDTO DecodeFenceReleased(const std::string &encoding) {
  const auto tokens = DecodeTokens(encoding, 9U, "fence released receipt");
  if (tokens.size() != 9U) {
    throw ManagerApplyJournalIntegrityError(
        "stored fence-released tuple cardinality is invalid");
  }
  ConfigurationFenceReleasedDTO released{
      DecodeFenceReleaseRequest(tokens[0]),
      core::ExecutionHostEpochDTO{
          RuntimeUnitId(tokens[1]),
          RuntimeEpoch(tokens[2]),
          RequestOwnerEpoch(tokens[3]),
          ContentDigest(tokens[4]),
          ActiveBundlePublicationGeneration(ParseCanonicalSqliteUnsigned(
              tokens[5], "released Kernel publication generation", true)),
      },
      static_cast<std::int64_t>(
          ParseCanonicalSqliteUnsigned(tokens[6], "released_at_ns", false)),
      DecodeAcceptedReplayStatus(tokens[7]),
      ContentDigest(tokens[8]),
  };
  if (!released.valid() || EncodeFenceReleased(released) != encoding) {
    throw ManagerApplyJournalIntegrityError(
        "stored fence release receipt is invalid or noncanonical");
  }
  return released;
}

struct EncodedFenceReleaseIntent final {
  std::int64_t apply_generation;
  std::string release_request;
  std::string digest;
};

EncodedFenceReleaseIntent
EncodeFenceReleaseIntent(const ManagerApplyFenceReleaseIntentDTO &intent) {
  if (!intent.valid()) {
    throw std::invalid_argument("Manager fence-release intent is invalid");
  }
  ValidateDigest(intent.scene_completion_digest.value(),
                 "scene_completion_digest");
  ValidateDigest(intent.fence_acceptance_digest.value(),
                 "fence_acceptance_digest");
  const auto &owner =
      intent.release_request.reconciliation_evidence.owner_transaction;
  EncodedFenceReleaseIntent encoded{
      CheckedSigned(owner.apply_generation.value(), "apply_generation"),
      EncodeFenceReleaseRequest(intent.release_request),
      {},
  };
  std::string record;
  AppendField(record, "domain", kFenceReleaseIntentDigestDomain);
  AppendField(record, "scene_completion_digest",
              intent.scene_completion_digest.value());
  AppendField(record, "fence_acceptance_digest",
              intent.fence_acceptance_digest.value());
  AppendField(record, "apply_id", owner.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "release_request_encoding", encoded.release_request);
  encoded.digest = RawDigest(record);
  return encoded;
}

struct EncodedFenceReleasedRecord final {
  std::int64_t apply_generation;
  std::string released_fence;
  std::string digest;
};

EncodedFenceReleasedRecord
EncodeFenceReleasedRecord(const ManagerApplyFenceReleasedDTO &record_value) {
  if (!record_value.valid()) {
    throw std::invalid_argument("Manager fence-released record is invalid");
  }
  ValidateDigest(record_value.fence_release_intent_digest.value(),
                 "fence_release_intent_digest");
  const auto &owner = record_value.released_fence.request
                          .reconciliation_evidence.owner_transaction;
  EncodedFenceReleasedRecord encoded{
      CheckedSigned(owner.apply_generation.value(), "apply_generation"),
      EncodeFenceReleased(record_value.released_fence),
      {},
  };
  std::string digest_record;
  AppendField(digest_record, "domain", kFenceReleasedDigestDomain);
  AppendField(digest_record, "fence_release_intent_digest",
              record_value.fence_release_intent_digest.value());
  AppendField(digest_record, "apply_id", owner.apply_id.value());
  AppendField(digest_record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(digest_record, "released_fence_encoding", encoded.released_fence);
  encoded.digest = RawDigest(digest_record);
  return encoded;
}

struct EncodedTerminalOutcome final {
  std::int64_t apply_generation;
  std::optional<std::int64_t> candidate_scene_revision_id;
  std::string state;
  std::string digest;
};

EncodedTerminalOutcome
EncodeTerminalOutcome(const ManagerApplyTerminalOutcomeDTO &outcome) {
  if (!outcome.valid()) {
    throw std::invalid_argument("Manager terminal Apply outcome is invalid");
  }
  ValidateBoundedIdentity(outcome.apply_id.value(), "apply_id");
  EncodedTerminalOutcome encoded{
      CheckedSigned(outcome.apply_generation.value(), "apply_generation"),
      std::nullopt,
      ToString(outcome.state),
      {},
  };
  std::string record;
  AppendField(record, "domain", kTerminalOutcomeDigestDomain);
  AppendField(record, "apply_id", outcome.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "outcome_state", encoded.state);
  if (outcome.state == ManagerApplyTerminalOutcomeState::kSucceeded) {
    ValidateDigest(outcome.fence_released_digest->value(),
                   "fence_released_digest");
    ValidateDigest(outcome.scene_completion_digest->value(),
                   "scene_completion_digest");
    ValidateDigest(outcome.publication_receipt_digest->value(),
                   "publication_receipt_digest");
    const auto &candidate = *outcome.candidate_projection_revision;
    encoded.candidate_scene_revision_id = CheckedSigned(
        candidate.scene_revision.revision_id, "candidate_scene_revision_id");
    AppendField(record, "fence_released_digest",
                outcome.fence_released_digest->value());
    AppendField(record, "scene_completion_digest",
                outcome.scene_completion_digest->value());
    AppendField(record, "publication_receipt_digest",
                outcome.publication_receipt_digest->value());
    AppendField(record, "candidate_retained_bundle_id",
                candidate.retained_bundle_id.value());
    AppendField(record, "candidate_graph_revision",
                candidate.graph_revision.value());
    AppendField(record, "candidate_scene_id",
                candidate.scene_revision.scene_id.value());
    AppendField(record, "candidate_scene_revision_id",
                std::to_string(*encoded.candidate_scene_revision_id));
    AppendField(record, "candidate_scene_content_hash",
                candidate.scene_revision.content_hash.value());
  } else {
    const auto &failure = *outcome.failure;
    ValidateDigest(failure.intent_digest.value(), "intent_digest");
    ValidateBoundedText(failure.failure_code,
                        kMaximumManagerApplyFailureCodeBytes, "failure_code");
    ValidateBoundedText(failure.failure_message,
                        kMaximumManagerApplyFailureMessageBytes,
                        "failure_message");
    ValidateBoundedText(failure.cause_code,
                        kMaximumManagerApplyFailureCodeBytes, "cause_code");
    ValidateBoundedText(failure.producer,
                        kMaximumManagerApplyFailureProducerBytes, "producer");
    AppendField(record, "intent_digest", failure.intent_digest.value());
    AppendField(record, "failure_phase", ToString(failure.phase));
    AppendField(record, "failure_code", failure.failure_code);
    AppendField(record, "failure_message", failure.failure_message);
    AppendField(record, "effect_disposition",
                ToString(failure.effect_disposition));
    AppendField(record, "cause_code", failure.cause_code);
    AppendField(record, "cause_details",
                EncodeCauseDetails(failure.cause_details));
    AppendField(record, "producer", failure.producer);
    AppendField(record, "effect_boundary", ToString(failure.effect_boundary));
  }
  encoded.digest = RawDigest(record);
  return encoded;
}

[[nodiscard]] ManagerApplyTerminalOutcomeState ParseTerminalOutcomeState(
    const std::string_view value) {
  if (value == "succeeded") {
    return ManagerApplyTerminalOutcomeState::kSucceeded;
  }
  if (value == "failed") {
    return ManagerApplyTerminalOutcomeState::kFailed;
  }
  if (value == "unknown_effect") {
    return ManagerApplyTerminalOutcomeState::kUnknownEffect;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored terminal Apply outcome has an unknown state");
}

[[nodiscard]] ManagerApplyPhase ParseManagerApplyPhase(
    const std::string_view value) {
  constexpr std::array<ManagerApplyPhase, 7U> kPhases{
      ManagerApplyPhase::kAccepted,
      ManagerApplyPhase::kFenceAcquired,
      ManagerApplyPhase::kFenceAccepted,
      ManagerApplyPhase::kRuntimeConverged,
      ManagerApplyPhase::kComposed,
      ManagerApplyPhase::kPublished,
      ManagerApplyPhase::kSettled,
  };
  for (const auto phase : kPhases) {
    if (value == ToString(phase)) {
      return phase;
    }
  }
  throw ManagerApplyJournalIntegrityError(
      "stored terminal Apply failure has an unknown phase");
}

[[nodiscard]] ManagerApplyEffectDisposition ParseEffectDisposition(
    const std::string_view value) {
  if (value == "known") {
    return ManagerApplyEffectDisposition::kKnown;
  }
  if (value == "unknown") {
    return ManagerApplyEffectDisposition::kUnknown;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored terminal Apply failure has an unknown effect disposition");
}

[[nodiscard]] ManagerApplyEffectBoundary ParseEffectBoundary(
    const std::string_view value) {
  if (value == "before_effect") {
    return ManagerApplyEffectBoundary::kBeforeEffect;
  }
  if (value == "unknown") {
    return ManagerApplyEffectBoundary::kUnknown;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored terminal Apply failure has an unknown effect boundary");
}

bool SameFenceIntent(const ApplyConfigurationFenceIntentDTO &lhs,
                     const ApplyConfigurationFenceIntentDTO &rhs) {
  return lhs.owner_request_context == rhs.owner_request_context &&
         lhs.owner_transaction == rhs.owner_transaction &&
         lhs.scope == rhs.scope &&
         lhs.busy_disposition == rhs.busy_disposition &&
         lhs.expected_retained_bundle_id == rhs.expected_retained_bundle_id &&
         lhs.expected_graph_revision == rhs.expected_graph_revision &&
         lhs.expected_operation_resource_generation ==
             rhs.expected_operation_resource_generation &&
         lhs.expected_fence_generation == rhs.expected_fence_generation &&
         lhs.expected_active_control_configurations ==
             rhs.expected_active_control_configurations &&
         lhs.reason == rhs.reason;
}

struct EncodedAffectedScope final {
  std::int64_t apply_generation;
  std::string components;
  std::string domains;
  std::string bindings;
  std::string resources;
  std::string busy_disposition;
  std::string component_delta;
  std::string digest;
};

EncodedAffectedScope
EncodeAffectedScope(const ContentDigest &intent_digest,
                    const ManagerApplyAffectedScopeDTO &affected_scope) {
  if (affected_scope.scope.controllable_component_ids.size() >
          kMaximumManagerApplyFenceComponents ||
      affected_scope.scope.domains.size() > kMaximumManagerApplyFenceDomains ||
      affected_scope.scope.bindings.size() >
          kMaximumManagerApplyFenceBindings ||
      affected_scope.scope.resources.size() >
          kMaximumManagerApplyFenceResources) {
    throw ManagerApplyJournalLimitError(
        "Apply affected scope exceeds a fixed cardinality limit");
  }
  if (!affected_scope.valid()) {
    throw std::invalid_argument("Apply affected scope is invalid");
  }
  ValidateDigest(intent_digest.value(), "intent_digest");
  ValidateBoundedIdentity(affected_scope.apply_id.value(), "apply_id");
  EncodedAffectedScope encoded{
      CheckedSigned(affected_scope.apply_generation.value(),
                    "apply_generation"),
      EncodeComponents(affected_scope.scope.controllable_component_ids),
      EncodeDomains(affected_scope.scope),
      EncodeBindings(affected_scope.scope.bindings),
      EncodeResources(affected_scope.scope.resources),
      std::string(core::to_string(affected_scope.busy_disposition)),
      manyforge::deployment::EncodeApplyComponentDelta(
          affected_scope.component_delta),
      {},
  };
  std::string record;
  AppendField(record, "domain", kAffectedScopeDigestDomain);
  AppendField(record, "intent_digest", intent_digest.value());
  AppendField(record, "apply_id", affected_scope.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "components_encoding", encoded.components);
  AppendField(record, "domains_encoding", encoded.domains);
  AppendField(record, "bindings_encoding", encoded.bindings);
  AppendField(record, "resources_encoding", encoded.resources);
  AppendField(record, "busy_disposition", encoded.busy_disposition);
  AppendField(record, "component_delta_encoding", encoded.component_delta);
  encoded.digest = RawDigest(record);
  return encoded;
}

bool SameAcceptedFence(const ConfigurationFenceAcceptedDTO &lhs,
                       const ConfigurationFenceAcceptedDTO &rhs) {
  return lhs.configuration_fence_id == rhs.configuration_fence_id &&
         lhs.configuration_fence_generation ==
             rhs.configuration_fence_generation &&
         lhs.captured_scope == rhs.captured_scope &&
         lhs.accepted_at_ns == rhs.accepted_at_ns &&
         lhs.replay_status == rhs.replay_status;
}

struct EncodedFenceAttempt final {
  std::int64_t apply_generation;
  std::int64_t operation_generation;
  std::int64_t fence_generation;
  std::string components;
  std::string domains;
  std::string bindings;
  std::string resources;
  std::string active_configurations;
  std::string digest;
};

EncodedFenceAttempt
EncodeFenceAttempt(const ContentDigest &intent_digest,
                   const ContentDigest &affected_scope_digest,
                   const ApplyConfigurationFenceIntentDTO &intent) {
  if (intent.scope.controllable_component_ids.size() >
          kMaximumManagerApplyFenceComponents ||
      intent.scope.domains.size() > kMaximumManagerApplyFenceDomains ||
      intent.scope.bindings.size() > kMaximumManagerApplyFenceBindings ||
      intent.scope.resources.size() > kMaximumManagerApplyFenceResources) {
    throw ManagerApplyJournalLimitError(
        "Apply configuration-fence scope exceeds a fixed cardinality limit");
  }
  if (!intent.valid()) {
    throw std::invalid_argument("Apply configuration-fence intent is invalid");
  }
  ValidateDigest(intent_digest.value(), "intent_digest");
  ValidateDigest(affected_scope_digest.value(), "affected_scope_digest");
  ValidateBoundedIdentity(intent.owner_request_context.request_id.value(),
                          "request_id");
  ValidateBoundedIdentity(
      intent.owner_request_context.request_owner_epoch.value(),
      "request_owner_epoch");
  ValidateBoundedText(intent.owner_request_context.source_label,
                      kMaximumManagerApplySourceLabelBytes, "source_label");
  if (intent.owner_request_context.correlation_id.has_value()) {
    ValidateBoundedText(*intent.owner_request_context.correlation_id,
                        kMaximumManagerApplyCorrelationIdBytes,
                        "correlation_id");
  }
  ValidateBoundedIdentity(intent.owner_transaction.apply_id.value(),
                          "apply_id");
  ValidateBoundedIdentity(intent.expected_retained_bundle_id.value(),
                          "expected_retained_bundle_id");
  ValidateBoundedIdentity(intent.expected_graph_revision.value(),
                          "expected_graph_revision");
  ValidateBoundedText(intent.reason, kMaximumManagerApplyFenceReasonBytes,
                      "fence reason");
  EncodedFenceAttempt encoded{
      CheckedSigned(intent.owner_transaction.apply_generation.value(),
                    "apply_generation"),
      CheckedNonnegativeSigned(intent.expected_operation_resource_generation,
                               "expected_operation_resource_generation"),
      CheckedNonnegativeSigned(intent.expected_fence_generation.value(),
                               "expected_fence_generation"),
      EncodeComponents(intent.scope.controllable_component_ids),
      EncodeDomains(intent.scope),
      EncodeBindings(intent.scope.bindings),
      EncodeResources(intent.scope.resources),
      EncodeActiveConfigurations(intent.expected_active_control_configurations),
      {},
  };
  std::string record;
  AppendField(record, "domain", kFenceAttemptDigestDomain);
  AppendField(record, "intent_digest", intent_digest.value());
  AppendField(record, "affected_scope_digest", affected_scope_digest.value());
  AppendField(record, "apply_id", intent.owner_transaction.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "request_id",
              intent.owner_request_context.request_id.value());
  AppendField(record, "request_owner_epoch",
              intent.owner_request_context.request_owner_epoch.value());
  AppendField(record, "source_label",
              intent.owner_request_context.source_label);
  AppendField(record, "correlation_id_present",
              intent.owner_request_context.correlation_id ? "1" : "0");
  AppendField(record, "correlation_id",
              intent.owner_request_context.correlation_id.value_or(""));
  AppendField(record, "components_encoding", encoded.components);
  AppendField(record, "domains_encoding", encoded.domains);
  AppendField(record, "bindings_encoding", encoded.bindings);
  AppendField(record, "resources_encoding", encoded.resources);
  AppendField(record, "busy_disposition",
              core::to_string(intent.busy_disposition));
  AppendField(record, "expected_retained_bundle_id",
              intent.expected_retained_bundle_id.value());
  AppendField(record, "expected_graph_revision",
              intent.expected_graph_revision.value());
  AppendField(record, "expected_operation_resource_generation",
              std::to_string(encoded.operation_generation));
  AppendField(record, "expected_fence_generation",
              std::to_string(encoded.fence_generation));
  AppendField(record, "active_configurations_encoding",
              encoded.active_configurations);
  AppendField(record, "reason", intent.reason);
  encoded.digest = RawDigest(record);
  return encoded;
}

struct EncodedFenceAcceptance final {
  std::int64_t apply_generation;
  std::int64_t fence_generation;
  std::string components;
  std::string domains;
  std::string bindings;
  std::string resources;
  std::string replay_status;
  std::string digest;
};

EncodedFenceAcceptance
EncodeFenceAcceptance(const ApplyId &apply_id,
                      const ApplyGeneration apply_generation,
                      const ContentDigest &fence_attempt_digest,
                      const ConfigurationFenceAcceptedDTO &accepted) {
  if (accepted.captured_scope.controllable_component_ids.size() >
          kMaximumManagerApplyFenceComponents ||
      accepted.captured_scope.domains.size() >
          kMaximumManagerApplyFenceDomains ||
      accepted.captured_scope.bindings.size() >
          kMaximumManagerApplyFenceBindings ||
      accepted.captured_scope.resources.size() >
          kMaximumManagerApplyFenceResources) {
    throw ManagerApplyJournalLimitError(
        "accepted configuration-fence scope exceeds a fixed limit");
  }
  if (!accepted.valid()) {
    throw std::invalid_argument("configuration-fence acceptance is invalid");
  }
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  ValidateDigest(fence_attempt_digest.value(), "fence_attempt_digest");
  ValidateBoundedIdentity(accepted.configuration_fence_id.value(),
                          "configuration_fence_id");
  EncodedFenceAcceptance encoded{
      CheckedSigned(apply_generation.value(), "apply_generation"),
      CheckedSigned(accepted.configuration_fence_generation.value(),
                    "configuration_fence_generation"),
      EncodeComponents(accepted.captured_scope.controllable_component_ids),
      EncodeDomains(accepted.captured_scope),
      EncodeBindings(accepted.captured_scope.bindings),
      EncodeResources(accepted.captured_scope.resources),
      std::string(core::to_string(accepted.replay_status)),
      {},
  };
  std::string record;
  AppendField(record, "domain", kFenceAcceptanceDigestDomain);
  AppendField(record, "fence_attempt_digest", fence_attempt_digest.value());
  AppendField(record, "apply_id", apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "configuration_fence_id",
              accepted.configuration_fence_id.value());
  AppendField(record, "configuration_fence_generation",
              std::to_string(encoded.fence_generation));
  AppendField(record, "captured_components_encoding", encoded.components);
  AppendField(record, "captured_domains_encoding", encoded.domains);
  AppendField(record, "captured_bindings_encoding", encoded.bindings);
  AppendField(record, "captured_resources_encoding", encoded.resources);
  AppendField(record, "accepted_at_ns",
              std::to_string(accepted.accepted_at_ns));
  AppendField(record, "replay_status", encoded.replay_status);
  encoded.digest = RawDigest(record);
  return encoded;
}

std::string
EncodeRuntimeUnitIds(const std::vector<RuntimeUnitId> &runtime_unit_ids) {
  std::vector<std::string> tokens;
  tokens.reserve(runtime_unit_ids.size());
  for (const auto &runtime_unit_id : runtime_unit_ids) {
    ValidateBoundedIdentity(runtime_unit_id.value(), "runtime_unit_id");
    tokens.push_back(runtime_unit_id.value());
  }
  return EncodeTokens(tokens);
}

std::string
EncodeRuntimeRoleIds(const std::vector<RuntimeRoleId> &runtime_role_ids) {
  std::vector<std::string> tokens;
  tokens.reserve(runtime_role_ids.size());
  for (const auto &runtime_role_id : runtime_role_ids) {
    ValidateBoundedIdentity(runtime_role_id.value(), "runtime_role_id");
    tokens.push_back(runtime_role_id.value());
  }
  return EncodeTokens(tokens);
}

std::string EncodeRuntimePlanRows(
    const std::vector<RuntimeUnitConvergencePlanRowDTO> &runtime_units) {
  std::vector<std::string> tokens;
  tokens.reserve(runtime_units.size() * 27U);
  for (const auto &row : runtime_units) {
    if (!row.valid()) {
      throw std::invalid_argument("runtime convergence plan row is invalid");
    }
    tokens.push_back(row.runtime_unit_id.value());
    tokens.push_back(row.runtime_profile_id.value());
    tokens.push_back(row.runtime_profile_revision.value());
    tokens.emplace_back(core::to_string(row.lifecycle_control));
    tokens.emplace_back(ToString(row.candidate_disposition));
    tokens.push_back(row.product_id.value());
    tokens.push_back(row.product_contract_id.value());
    tokens.push_back(row.product_contract_revision.value());
    tokens.push_back(row.product_identity.value());
    tokens.push_back(row.configuration_identity.value());
    tokens.push_back(EncodeRuntimeUnitIds(row.dependency_runtime_unit_ids));
    tokens.push_back(EncodeRuntimeRoleIds(row.required_runtime_role_ids));
    tokens.push_back(row.status_endpoint_role_id.value());
    tokens.push_back(row.status_endpoint_contract_id);
    tokens.push_back(row.status_endpoint_contract_revision);
    tokens.push_back(row.status_transport);
    tokens.push_back(row.process_adapter_contract_id);
    tokens.push_back(row.process_adapter_contract_revision);
    tokens.push_back(row.retrieval_contract_id);
    tokens.push_back(row.retrieval_contract_revision);
    tokens.emplace_back(row.launchable_content_digest.has_value() ? "1" : "0");
    tokens.push_back(row.launchable_content_digest.has_value()
                         ? row.launchable_content_digest->value()
                         : std::string{});
    tokens.push_back(std::to_string(row.launchable_byte_size));
    tokens.push_back(row.operating_system);
    tokens.push_back(row.shell_contract);
    ValidateDigest(row.runtime_contract_offer_digest.value(),
                   "runtime contract offer digest");
    const auto requirement =
        core::EncodeRuntimeContractRequirement(
            row.runtime_contract_requirement);
    if (!requirement.ok()) {
      throw std::invalid_argument(
          "runtime convergence requirement encoding failed");
    }
    tokens.push_back(row.runtime_contract_offer_digest.value());
    tokens.push_back(requirement.value());
  }
  return EncodeTokens(tokens);
}

core::RuntimeLifecycleControl
DecodeRuntimeLifecycleControl(const std::string_view value) {
  if (value == "manager_managed") {
    return core::RuntimeLifecycleControl::kManagerManaged;
  }
  if (value == "externally_managed") {
    return core::RuntimeLifecycleControl::kExternallyManaged;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored runtime lifecycle control is invalid");
}

RuntimeUnitCandidateDisposition
DecodeRuntimeCandidateDisposition(const std::string_view value) {
  if (value == "added") {
    return RuntimeUnitCandidateDisposition::kAdded;
  }
  if (value == "retained_exact") {
    return RuntimeUnitCandidateDisposition::kRetainedExact;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored runtime candidate disposition is invalid");
}

std::vector<RuntimeUnitConvergencePlanRowDTO>
DecodeRuntimePlanRows(const std::string &encoding) {
  constexpr std::size_t kFieldsPerRow = 27U;
  const auto tokens =
      DecodeTokens(encoding, kMaximumRuntimeUnits * kFieldsPerRow,
                   "runtime convergence plan");
  if (tokens.size() % kFieldsPerRow != 0U) {
    throw ManagerApplyJournalIntegrityError(
        "stored runtime convergence plan tuple cardinality is invalid");
  }
  std::vector<RuntimeUnitConvergencePlanRowDTO> result;
  result.reserve(tokens.size() / kFieldsPerRow);
  for (std::size_t index = 0U; index < tokens.size(); index += kFieldsPerRow) {
    const auto dependency_tokens =
        DecodeTokens(tokens[index + 10U], kMaximumDependencies,
                     "runtime convergence dependencies");
    const auto role_tokens = DecodeTokens(
        tokens[index + 11U], kMaximumRuntimeRoles, "runtime convergence roles");
    std::vector<RuntimeUnitId> dependencies;
    std::vector<RuntimeRoleId> roles;
    for (const auto &dependency : dependency_tokens) {
      dependencies.emplace_back(dependency);
    }
    for (const auto &role : role_tokens) {
      roles.emplace_back(role);
    }
    std::optional<ContentDigest> launchable_digest;
    if (tokens[index + 20U] == "1") {
      if (tokens[index + 21U].empty()) {
        throw ManagerApplyJournalIntegrityError(
            "stored runtime launchable digest is absent");
      }
      launchable_digest.emplace(tokens[index + 21U]);
    } else if (tokens[index + 20U] != "0" || !tokens[index + 21U].empty()) {
      throw ManagerApplyJournalIntegrityError(
          "stored runtime launchable presence is invalid");
    }
    RuntimeUnitConvergencePlanRowDTO row{
        RuntimeUnitId(tokens[index]),
        RuntimeProfileId(tokens[index + 1U]),
        RuntimeProfileRevision(tokens[index + 2U]),
        DecodeRuntimeLifecycleControl(tokens[index + 3U]),
        DecodeRuntimeCandidateDisposition(tokens[index + 4U]),
        ProductId(tokens[index + 5U]),
        ProductContractId(tokens[index + 6U]),
        ProductContractRevision(tokens[index + 7U]),
        ContentDigest(tokens[index + 8U]),
        ContentDigest(tokens[index + 9U]),
        std::move(dependencies),
        std::move(roles),
        core::EndpointRoleId(tokens[index + 12U]),
        tokens[index + 13U],
        tokens[index + 14U],
        tokens[index + 15U],
        tokens[index + 16U],
        tokens[index + 17U],
        tokens[index + 18U],
        tokens[index + 19U],
        std::move(launchable_digest),
        ParseCanonicalUnsigned(tokens[index + 22U],
                               "runtime launchable byte size"),
        tokens[index + 23U],
        tokens[index + 24U],
        [&]() {
          const auto requirement = core::DecodeRuntimeContractRequirement(
              tokens[index + 26U]);
          if (!requirement.ok()) {
            throw ManagerApplyJournalIntegrityError(
                "stored runtime contract requirement is invalid");
          }
          return requirement.value();
        }(),
        core::RuntimeContractOfferDigest(tokens[index + 25U]),
    };
    if (!row.valid()) {
      throw ManagerApplyJournalIntegrityError(
          "stored runtime convergence plan row is invalid");
    }
    result.push_back(std::move(row));
  }
  return result;
}

std::string EncodeDependencyEpochs(
    const std::vector<RuntimeUnitDependencyEpochDTO> &dependencies) {
  std::vector<std::string> tokens;
  tokens.reserve(dependencies.size() * 2U);
  for (const auto &dependency : dependencies) {
    if (!dependency.valid()) {
      throw std::invalid_argument("runtime dependency epoch is invalid");
    }
    tokens.push_back(dependency.runtime_unit_id.value());
    tokens.push_back(dependency.runtime_epoch.value());
  }
  return EncodeTokens(tokens);
}

std::string EncodeRuntimeStagedReadyRows(
    const std::vector<RuntimeUnitStagedReadyEvidenceDTO> &runtime_units) {
  std::vector<std::string> tokens;
  tokens.reserve(runtime_units.size() * 19U);
  for (const auto &row : runtime_units) {
    if (!row.valid()) {
      throw std::invalid_argument("runtime staged-ready row is invalid");
    }
    tokens.push_back(row.runtime_unit_id.value());
    tokens.emplace_back(core::to_string(row.lifecycle_control));
    tokens.emplace_back(ToString(row.candidate_disposition));
    tokens.push_back(row.runtime_epoch.value());
    tokens.push_back(row.process_observer_instance_id);
    tokens.push_back(std::to_string(row.process_observation_sequence));
    tokens.push_back(std::to_string(row.source_report_sequence));
    tokens.push_back(std::to_string(row.critical_progress_sequence));
    tokens.push_back(row.observed_product_identity.value());
    tokens.push_back(row.observed_configuration_identity.value());
    tokens.emplace_back(ToString(row.source_phase));
    tokens.emplace_back(ToString(row.source_health));
    tokens.push_back(EncodeRuntimeRoleIds(row.ready_runtime_role_ids));
    tokens.emplace_back(ToString(row.command_surface_state));
    tokens.push_back(EncodeDependencyEpochs(row.dependency_runtime_epochs));
    tokens.push_back(std::to_string(row.presence_age_ns));
    tokens.push_back(std::to_string(row.report_age_ns));
    tokens.push_back(std::to_string(row.progress_age_ns));
    tokens.push_back(row.observed_runtime_contract_offer_digest.value());
  }
  return EncodeTokens(tokens);
}

RuntimeSourcePhase DecodeRuntimeSourcePhase(const std::string_view value) {
  if (value == "serving") {
    return RuntimeSourcePhase::kServing;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored staged-ready source phase is invalid");
}

RuntimeSourceHealth DecodeRuntimeSourceHealth(const std::string_view value) {
  if (value == "healthy") {
    return RuntimeSourceHealth::kHealthy;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored staged-ready source health is invalid");
}

ManagedCommandSurfaceState
DecodeRuntimeCommandSurface(const std::string_view value) {
  if (value == "closed_no_active_effect") {
    return ManagedCommandSurfaceState::kClosedNoActiveEffect;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored staged-ready command surface is invalid");
}

std::vector<RuntimeUnitStagedReadyEvidenceDTO>
DecodeRuntimeStagedReadyRows(const std::string &encoding) {
  constexpr std::size_t kFieldsPerRow = 19U;
  const auto tokens =
      DecodeTokens(encoding, kMaximumRuntimeUnits * kFieldsPerRow,
                   "runtime staged-ready evidence");
  if (tokens.size() % kFieldsPerRow != 0U) {
    throw ManagerApplyJournalIntegrityError(
        "stored runtime staged-ready tuple cardinality is invalid");
  }
  std::vector<RuntimeUnitStagedReadyEvidenceDTO> result;
  result.reserve(tokens.size() / kFieldsPerRow);
  for (std::size_t index = 0U; index < tokens.size(); index += kFieldsPerRow) {
    const auto role_tokens =
        DecodeTokens(tokens[index + 12U], kMaximumRuntimeRoles,
                     "runtime staged-ready roles");
    const auto dependency_tokens =
        DecodeTokens(tokens[index + 14U], kMaximumDependencies * 2U,
                     "runtime staged-ready dependency epochs");
    if (dependency_tokens.size() % 2U != 0U) {
      throw ManagerApplyJournalIntegrityError(
          "stored runtime dependency-epoch tuple cardinality is invalid");
    }
    std::vector<RuntimeRoleId> roles;
    std::vector<RuntimeUnitDependencyEpochDTO> dependencies;
    for (const auto &role : role_tokens) {
      roles.emplace_back(role);
    }
    for (std::size_t dependency = 0U; dependency < dependency_tokens.size();
         dependency += 2U) {
      dependencies.push_back(
          {RuntimeUnitId(dependency_tokens[dependency]),
           RuntimeEpoch(dependency_tokens[dependency + 1U])});
    }
    RuntimeUnitStagedReadyEvidenceDTO row{
        RuntimeUnitId(tokens[index]),
        DecodeRuntimeLifecycleControl(tokens[index + 1U]),
        DecodeRuntimeCandidateDisposition(tokens[index + 2U]),
        RuntimeEpoch(tokens[index + 3U]),
        tokens[index + 4U],
        ParseCanonicalUnsigned(tokens[index + 5U],
                               "process observation sequence"),
        ParseCanonicalUnsigned(tokens[index + 6U], "source report sequence"),
        ParseCanonicalUnsigned(tokens[index + 7U],
                               "critical progress sequence"),
        ContentDigest(tokens[index + 8U]),
        ContentDigest(tokens[index + 9U]),
        DecodeRuntimeSourcePhase(tokens[index + 10U]),
        DecodeRuntimeSourceHealth(tokens[index + 11U]),
        std::move(roles),
        DecodeRuntimeCommandSurface(tokens[index + 13U]),
        std::move(dependencies),
        ParseCanonicalUnsigned(tokens[index + 15U], "presence age"),
        ParseCanonicalUnsigned(tokens[index + 16U], "report age"),
        ParseCanonicalUnsigned(tokens[index + 17U], "progress age"),
        core::RuntimeContractOfferDigest(tokens[index + 18U]),
    };
    if (!row.valid()) {
      throw ManagerApplyJournalIntegrityError(
          "stored runtime staged-ready row is invalid");
    }
    result.push_back(std::move(row));
  }
  return result;
}

struct EncodedRuntimeConvergenceIntent final {
  std::int64_t apply_generation;
  std::int64_t predecessor_publication_generation;
  std::string runtime_units;
  std::string digest;
};

EncodedRuntimeConvergenceIntent EncodeRuntimeConvergenceIntent(
    const ContentDigest &intent_digest,
    const ManagerApplyRuntimeConvergenceIntentDTO &intent) {
  if (!intent.valid()) {
    throw std::invalid_argument("runtime convergence intent is invalid");
  }
  ValidateDigest(intent_digest.value(), "intent_digest");
  EncodedRuntimeConvergenceIntent encoded{
      CheckedSigned(intent.apply_generation.value(), "apply_generation"),
      CheckedSigned(intent.predecessor_publication_generation.value(),
                    "predecessor_publication_generation"),
      EncodeRuntimePlanRows(intent.runtime_units),
      {},
  };
  std::string record;
  AppendField(record, "domain", kRuntimeConvergenceIntentDigestDomain);
  AppendField(record, "intent_digest", intent_digest.value());
  AppendField(record, "apply_id", intent.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "candidate_retained_bundle_id",
              intent.candidate_retained_bundle_id.value());
  AppendField(record, "candidate_graph_revision",
              intent.candidate_graph_revision.value());
  AppendField(record, "fence_acceptance_digest",
              intent.fence_acceptance_digest.value());
  AppendField(record, "predecessor_publication_generation",
              std::to_string(encoded.predecessor_publication_generation));
  AppendField(record, "predecessor_public_pointer_digest",
              intent.predecessor_public_pointer_digest.value());
  AppendField(record, "runtime_units_encoding", encoded.runtime_units);
  encoded.digest = RawDigest(record);
  return encoded;
}

struct EncodedRuntimeStagedReady final {
  std::int64_t apply_generation;
  std::int64_t staged_ready_generation;
  std::int64_t predecessor_publication_generation;
  std::int64_t evaluated_at_ns;
  std::string runtime_evidence;
  std::string digest;
};

EncodedRuntimeStagedReady EncodeRuntimeStagedReady(
    const ManagerApplyRuntimeStagedReadyDTO &staged_ready) {
  if (!staged_ready.valid()) {
    throw std::invalid_argument("runtime staged-ready set is invalid");
  }
  EncodedRuntimeStagedReady encoded{
      CheckedSigned(staged_ready.apply_generation.value(), "apply_generation"),
      CheckedSigned(staged_ready.staged_ready_generation,
                    "staged_ready_generation"),
      CheckedSigned(staged_ready.predecessor_publication_generation.value(),
                    "predecessor_publication_generation"),
      CheckedSigned(staged_ready.evaluated_at_ns, "evaluated_at_ns"),
      EncodeRuntimeStagedReadyRows(staged_ready.runtime_units),
      {},
  };
  std::string record;
  AppendField(record, "domain", kRuntimeStagedReadyDigestDomain);
  AppendField(record, "runtime_convergence_intent_digest",
              staged_ready.runtime_convergence_intent_digest.value());
  AppendField(record, "apply_id", staged_ready.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "staged_ready_generation",
              std::to_string(encoded.staged_ready_generation));
  AppendField(record, "predecessor_publication_generation",
              std::to_string(encoded.predecessor_publication_generation));
  AppendField(record, "predecessor_public_pointer_digest",
              staged_ready.predecessor_public_pointer_digest.value());
  AppendField(record, "evaluated_at_ns",
              std::to_string(encoded.evaluated_at_ns));
  AppendField(record, "runtime_evidence_encoding", encoded.runtime_evidence);
  encoded.digest = RawDigest(record);
  return encoded;
}

struct EncodedPrivateMaterialization final {
  std::int64_t apply_generation;
  std::int64_t candidate_scene_revision_id;
  std::int64_t retained_bundle_canonical_byte_size;
  std::string digest;
};

EncodedPrivateMaterialization EncodePrivateMaterialization(
    const ManagerApplyPrivateMaterializationDTO &materialization) {
  if (!materialization.valid()) {
    throw std::invalid_argument(
        "private selected-bundle materialization is invalid");
  }
  EncodedPrivateMaterialization encoded{
      CheckedSigned(materialization.apply_generation.value(),
                    "apply_generation"),
      CheckedSigned(materialization.candidate_projection_revision.scene_revision
                        .revision_id,
                    "candidate_scene_revision_id"),
      CheckedSigned(materialization.retained_bundle_canonical_byte_size,
                    "retained_bundle_canonical_byte_size"),
      {},
  };
  std::string record;
  AppendField(record, "domain", kPrivateMaterializationDigestDomain);
  AppendField(record, "authoritative_scene_snapshot_digest",
              materialization.authoritative_scene_snapshot_digest.value());
  AppendField(record, "apply_id", materialization.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  const auto &candidate = materialization.candidate_projection_revision;
  AppendField(record, "candidate_retained_bundle_id",
              candidate.retained_bundle_id.value());
  AppendField(record, "candidate_graph_revision",
              candidate.graph_revision.value());
  AppendField(record, "candidate_scene_id",
              candidate.scene_revision.scene_id.value());
  AppendField(record, "candidate_scene_revision_id",
              std::to_string(encoded.candidate_scene_revision_id));
  AppendField(record, "candidate_scene_content_hash",
              candidate.scene_revision.content_hash.value());
  AppendField(record, "retained_bundle_canonical_content_digest",
              materialization.retained_bundle_canonical_content_digest.value());
  AppendField(record, "retained_bundle_canonical_byte_size",
              std::to_string(encoded.retained_bundle_canonical_byte_size));
  encoded.digest = RawDigest(record);
  return encoded;
}

struct EncodedPublicationReceipt final {
  std::int64_t apply_generation;
  std::int64_t candidate_scene_revision_id;
  std::int64_t expected_previous_publication_generation;
  std::int64_t publication_generation;
  std::string digest;
};

EncodedPublicationReceipt
EncodePublicationReceipt(const ManagerApplyPublicationReceiptDTO &publication) {
  if (!publication.valid()) {
    throw std::invalid_argument("Manager Apply publication receipt is invalid");
  }
  const auto &receipt = publication.publication_receipt;
  const auto *transaction = std::get_if<NamedApplyPublicationTransactionDTO>(
      &receipt.request.transaction_ref);
  if (transaction == nullptr ||
      !receipt.request.expected_previous_publication_generation.has_value()) {
    throw std::invalid_argument("Manager Apply publication is not named-Apply");
  }
  EncodedPublicationReceipt encoded{
      CheckedSigned(transaction->apply_generation.value(), "apply_generation"),
      CheckedSigned(receipt.request.candidate_projection_revision.scene_revision
                        .revision_id,
                    "candidate_scene_revision_id"),
      CheckedSigned(
          receipt.request.expected_previous_publication_generation->value(),
          "expected_previous_publication_generation"),
      CheckedSigned(receipt.publication_generation.value(),
                    "publication_generation"),
      {},
  };
  std::string record;
  AppendField(record, "domain", kPublicationReceiptDigestDomain);
  AppendField(record, "private_materialization_content_digest",
              publication.private_materialization_content_digest.value());
  AppendField(record, "apply_id", transaction->apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  const auto &candidate = receipt.request.candidate_projection_revision;
  AppendField(record, "candidate_retained_bundle_id",
              candidate.retained_bundle_id.value());
  AppendField(record, "candidate_graph_revision",
              candidate.graph_revision.value());
  AppendField(record, "candidate_scene_id",
              candidate.scene_revision.scene_id.value());
  AppendField(record, "candidate_scene_revision_id",
              std::to_string(encoded.candidate_scene_revision_id));
  AppendField(record, "candidate_scene_content_hash",
              candidate.scene_revision.content_hash.value());
  AppendField(record, "expected_previous_publication_generation",
              std::to_string(encoded.expected_previous_publication_generation));
  AppendField(record, "publication_generation",
              std::to_string(encoded.publication_generation));
  AppendField(record, "durable_public_pointer_content_digest",
              receipt.durable_public_pointer_content_digest.value());
  encoded.digest = RawDigest(record);
  return encoded;
}

std::filesystem::path ValidateDatabasePath(std::filesystem::path path) {
  if (path.empty()) {
    throw std::invalid_argument(
        "manager Apply-journal database path cannot be empty");
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (!error && status.type() != std::filesystem::file_type::not_found) {
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
      throw std::invalid_argument(
          "manager Apply-journal database must be a non-symlink regular "
          "file");
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error ||
        size > static_cast<std::uintmax_t>(
                   kMaximumManagerApplyJournalDatabasePageImageBytes)) {
      throw ManagerApplyJournalLimitError(
          "Apply-journal database file exceeds its ceiling");
    }
  } else if (error && error != std::make_error_code(
                                   std::errc::no_such_file_or_directory)) {
    throw std::invalid_argument(
        "cannot inspect manager Apply-journal database path");
  }

  error.clear();
  const auto parent =
      path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
  if (!std::filesystem::exists(parent, error) ||
      !std::filesystem::is_directory(parent, error)) {
    throw std::invalid_argument("Apply-journal database parent must exist");
  }
  return path;
}

void ValidateApplyJournalMetadata(sqlite3 *database, const CellId &cell_id) {
  Statement row(database, "SELECT * FROM apply_journal_metadata");
  if (!row.Step()) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal metadata singleton is missing");
  }
  if (row.Integer(0) != 1 || row.Text(1) != kManagerApplyJournalSchemaId ||
      row.Integer(2) != kManagerApplyJournalSchemaVersion ||
      row.Text(3) != RawDigest(detail::kEmbeddedManagerApplyJournalSchema)) {
    throw ManagerApplyJournalIntegrityError("Apply-journal metadata mismatch");
  }
  if (row.Text(4) != cell_id.value()) {
    throw ManagerApplyJournalConflictError(
        "Apply journal belongs to a different cell");
  }
  if (row.Integer(5) !=
          static_cast<std::int64_t>(kMaximumManagerApplyIntents) ||
      row.Integer(6) !=
          static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts) ||
      row.Integer(7) !=
          static_cast<std::int64_t>(kMaximumManagerApplyAffectedScopes) ||
      row.Integer(8) !=
          static_cast<std::int64_t>(kMaximumManagerApplyFenceComponents) ||
      row.Integer(9) !=
          static_cast<std::int64_t>(kMaximumManagerApplyFenceDomains) ||
      row.Integer(10) !=
          static_cast<std::int64_t>(kMaximumManagerApplyFenceBindings) ||
      row.Integer(11) !=
          static_cast<std::int64_t>(kMaximumManagerApplyFenceResources) ||
      row.Integer(12) !=
          static_cast<std::int64_t>(kMaximumManagerApplyIntents) ||
      row.Integer(13) !=
          static_cast<std::int64_t>(kMaximumRuntimeStagedReadySets) ||
      row.Integer(14) != static_cast<std::int64_t>(
                             kMaximumManagerApplyPrivateMaterializations) ||
      row.Integer(15) !=
          static_cast<std::int64_t>(kMaximumManagerApplyPublicationReceipts) ||
      row.Integer(16) !=
          static_cast<std::int64_t>(kMaximumManagerApplyFenceReleaseIntents) ||
      row.Integer(17) !=
          static_cast<std::int64_t>(kMaximumManagerApplyFenceReleasedRecords) ||
      row.Integer(18) !=
          static_cast<std::int64_t>(kMaximumManagerApplyTerminalOutcomes) ||
      row.Integer(19) != kMaximumManagerApplyJournalDatabasePageImageBytes) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal resource-limit metadata mismatch");
  }
  if (row.Step()) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal metadata singleton is invalid");
  }
}

void ValidateApplyJournalIdentityAndSchema(
    sqlite3 *database, const CellId &cell_id,
    const std::filesystem::path &source_path = {}) {
  const auto existing = ReadSchemaObjects(database);
  const auto application_id = ScalarInteger(database, "PRAGMA application_id");
  const auto user_version = ScalarInteger(database, "PRAGMA user_version");
  if (existing.empty()) {
    if (application_id != 0 || user_version != 0) {
      throw ManagerApplyJournalIntegrityError(
          "schema-empty Apply-journal database has foreign SQLite "
          "identity");
    }
    return;
  }
  if (application_id != kManagerApplyJournalSqliteApplicationId) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal SQLite application ID mismatch");
  }
  if (user_version != kManagerApplyJournalSchemaVersion) {
    throw ManagerApplyJournalIntegrityError(
        "Manager Apply journal " +
        (source_path.empty()
             ? std::string(sqlite3_db_filename(database, "main"))
             : source_path.string()) +
        " has format version " + std::to_string(user_version) +
        "; expected v" + std::to_string(kManagerApplyJournalSchemaVersion) +
        ". Preserve this file, including interrupted operations; "
        "reconcile the original operations with their owner and retained "
        "evidence "
        "before explicitly selecting a fresh journal. No migration, "
        "conversion "
        "or deletion is performed");
  }
  if (existing != ExpectedSchemaObjects()) {
    throw ManagerApplyJournalIntegrityError(
        "SQLite schema objects differ from the versioned Apply artifact");
  }
  ValidateApplyJournalMetadata(database, cell_id);
}

void PreflightApplyJournal(const std::filesystem::path &database_path,
                           const CellId &cell_id) {
  try {
    static_cast<void>(persistence::PreflightExistingSqliteReadOnly(
        database_path,
        {
            static_cast<std::uintmax_t>(
                kMaximumManagerApplyJournalDatabasePageImageBytes),
            static_cast<std::uintmax_t>(
                kMaximumManagerApplyJournalDatabasePageImageBytes),
        },
        [&cell_id, &database_path](sqlite3 *database) {
          ValidateApplyJournalIdentityAndSchema(database, cell_id,
                                                database_path);
        }));
  } catch (const persistence::SqliteReadOnlyPreflightError &error) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal read-only preflight failed: " +
        std::string(error.what()));
  }
}

} // namespace

ContentDigest
ComputeManagerApplyIntentDigest(const ManagerApplyIntentDTO &intent) {
  return ContentDigest(EncodeIntent(intent).intent_digest);
}

ContentDigest ComputeManagerApplyPrivateMaterializationDigest(
    const ManagerApplyPrivateMaterializationDTO &materialization) {
  return ContentDigest(EncodePrivateMaterialization(materialization).digest);
}

bool ManagerApplyPreparedActivationDTO::valid() const noexcept {
  return core::IsExactLowercaseSha256Digest(captured_input_digest.value()) &&
         !deployment_artifact_id.empty() &&
         core::IsExactLowercaseSha256Digest(deployment_revision.value()) &&
         (!program_binding_id.has_value() ||
          core::IsExactLowercaseSha256Digest(program_binding_id->value())) &&
         core::IsExactLowercaseSha256Digest(prepared_activation_id.value()) &&
         !canonical_receipt_json.empty() &&
         canonical_receipt_json.size() <=
             kMaximumManagerApplyPreparedReceiptBytes;
}

const char *ToString(const ManagerApplyPhase phase) noexcept {
  switch (phase) {
  case ManagerApplyPhase::kAccepted:
    return "accepted";
  case ManagerApplyPhase::kFenceAcquired:
    return "fence_acquired";
  case ManagerApplyPhase::kFenceAccepted:
    return "fence_accepted";
  case ManagerApplyPhase::kRuntimeConverged:
    return "runtime_converged";
  case ManagerApplyPhase::kComposed:
    return "composed";
  case ManagerApplyPhase::kPublished:
    return "published";
  case ManagerApplyPhase::kSettled:
    return "settled";
  }
  return "unknown";
}

// Durable evidence remains valid if the set of failures offered to operators
// is narrowed in a later version. Scope policy belongs at the attempt boundary.
// A failed Apply that still holds its accepted fence and published nothing:
// releasable before effect (acknowledgement) or, whatever it changed, by New
// (N7, CONTRACTS §15).
static bool HasFailedApplyReleaseEvidence(const PersistedManagerApplyStatus &status) noexcept {
  if (!status.terminal_outcome || !status.affected_scope || !status.fence_attempt ||
      !status.fence_acceptance || status.publication_receipt ||
      status.fence_release_intent || status.fence_released ||
      !status.affected_scope->affected_scope.component_delta.valid())
    return false;
  const auto &terminal = status.terminal_outcome->terminal_outcome;
  return terminal.state != ManagerApplyTerminalOutcomeState::kSucceeded && terminal.failure;
}

// Runtime staging is no effect: a host that refused the staged candidate
// installed and published nothing (its failure is known before-effect).
bool FailedApplyAcknowledgementEligible(const PersistedManagerApplyStatus &status) noexcept {
  // The maintainer approved all proven-before-effect component/Scene changes
  // (DX-2814).
  if (!HasFailedApplyReleaseEvidence(status) || status.private_materialization) return false;
  const auto &terminal = status.terminal_outcome->terminal_outcome;
  return terminal.state == ManagerApplyTerminalOutcomeState::kFailed &&
         terminal.failure->effect_disposition == ManagerApplyEffectDisposition::kKnown &&
         terminal.failure->effect_boundary == ManagerApplyEffectBoundary::kBeforeEffect;
}

bool FailedApplyNewReleaseEligible(const PersistedManagerApplyStatus &status) noexcept {
  return HasFailedApplyReleaseEvidence(status);
}

ManagerApplyProgressDTO DeriveManagerApplyProgress(
    const PersistedManagerApplyStatus &status) {
  if (status.terminal_outcome.has_value()) {
    const auto &terminal = *status.terminal_outcome;
    return {terminal.terminal_outcome.failure.has_value()
                ? terminal.terminal_outcome.failure->phase
                : ManagerApplyPhase::kSettled,
            status.terminal_outcome->terminal_outcome_digest};
  }
  if (status.fence_released.has_value()) {
    return {ManagerApplyPhase::kSettled,
            status.fence_released->fence_released_digest};
  }
  if (status.publication_receipt.has_value()) {
    return {ManagerApplyPhase::kPublished,
            status.publication_receipt->publication_receipt_digest};
  }
  if (status.private_materialization.has_value()) {
    return {ManagerApplyPhase::kComposed,
            status.private_materialization
                ->private_materialization_content_digest};
  }
  if (status.latest_runtime_staged_ready.has_value()) {
    return {ManagerApplyPhase::kRuntimeConverged,
            status.latest_runtime_staged_ready->staged_ready_digest};
  }
  if (status.fence_acceptance.has_value()) {
    return {ManagerApplyPhase::kFenceAccepted,
            status.fence_acceptance->acceptance_digest};
  }
  if (status.affected_scope.has_value()) {
    return {ManagerApplyPhase::kFenceAcquired,
            status.affected_scope->affected_scope_digest};
  }
  return {ManagerApplyPhase::kAccepted, status.intent.intent_digest};
}

const char *ToString(const ManagerSceneInitialization value) noexcept {
  switch (value) {
  case ManagerSceneInitialization::kPreserve:
    return "preserve";
  case ManagerSceneInitialization::kInitialize:
    return "initialize";
  }
  return "invalid";
}

bool ManagerApplyIntentDTO::valid() const noexcept {
  const bool direct = expected_admission_generation.has_value() &&
                      expected_admission_generation->nonzero() &&
                      !parent_authoring_claim.has_value();
  const bool parent = !expected_admission_generation.has_value() &&
                      parent_authoring_claim.has_value() &&
                      core::ValidateCellAdmissionClaimIntent(
                          parent_authoring_claim->intent)
                          .ok() &&
                      parent_authoring_claim->intent.claim_kind ==
                          core::CellAdmissionClaimKindDTO::kAuthoringMutation &&
                      parent_authoring_claim->intent.owner_role ==
                          core::CellAdmissionOwnerRoleDTO::kAuthoringOwner;
  return (scene_initialization == ManagerSceneInitialization::kPreserve ||
          scene_initialization == ManagerSceneInitialization::kInitialize) &&
         (direct || parent) && !request_context.request_id.empty() &&
         !request_context.request_owner_epoch.empty() &&
         !request_context.source_label.empty() &&
         (!request_context.correlation_id.has_value() ||
          !request_context.correlation_id->empty()) &&
         !apply_id.empty() && apply_generation.nonzero() && !cell_id.empty() &&
         expected_active_projection_revision.valid() &&
         !candidate_retained_bundle_id.empty() &&
         !candidate_graph_revision.empty() && prepared_activation.valid();
}

class SqliteManagerApplyJournal::Impl final {
public:
  Impl(std::filesystem::path database_path, CellId cell_id)
      : database_path_(ValidateDatabasePath(std::move(database_path))),
        cell_id_(std::move(cell_id)) {
    ValidateBoundedIdentity(cell_id_.value(), "cell_id");
    if (sqlite3_libversion_number() < 3037000) {
      throw ManagerApplyJournalError(
          "manyforge_component_manager requires SQLite 3.37.0 or newer");
    }
    auto startup_lock =
        sqlite_startup_internal::TryAcquireManagerSqliteStoreStartupLock(
            std::chrono::milliseconds(kBusyTimeoutMilliseconds));
    if (!startup_lock.owns_lock()) {
      throw ManagerApplyJournalBusyError(
          "timed out waiting for process-local Apply-journal startup");
    }
    PreflightApplyJournal(database_path_, cell_id_);
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                      SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
    const auto result = sqlite3_open_v2(database_path_.string().c_str(),
                                        &database_, flags, nullptr);
    if (result != SQLITE_OK) {
      const std::string error = database_ == nullptr
                                    ? sqlite3_errstr(result)
                                    : sqlite3_errmsg(database_);
      if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
      }
      throw ManagerApplyJournalError("cannot open Apply-journal database: " +
                                     error);
    }
    try {
      Check(database_,
            sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds),
            "configure SQLite busy timeout");
      Configure();
      // Read schema and SQLite identity from one serialized WAL snapshot.  A
      // non-transactional first-open check can otherwise interleave those
      // reads with a peer's schema commit and misclassify that legitimate
      // initializer as a foreign database.
      Transaction validation(database_, "BEGIN EXCLUSIVE");
      ValidateApplyJournalIdentityAndSchema(database_, cell_id_,
                                            database_path_);
      validation.Commit();
      InitializeOrValidateSchema();
      Transaction transaction(database_, "BEGIN");
      static_cast<void>(VerifyIntegrityLocked());
      transaction.Commit();
    } catch (...) {
      sqlite3_close(database_);
      database_ = nullptr;
      throw;
    }
  }

  // A non-owning view of the already-open read-only settlement snapshot.
  // It reuses the exact uncached decoders and never configures or closes it.
  Impl(sqlite3 *borrowed_database, const CellId &cell_id)
      : cell_id_(cell_id), database_(borrowed_database), borrowed_(true) {}

  ~Impl() {
    if (borrowed_)
      return;
    try {
      Close();
    } catch (...) {
    }
  }

  void RequireOpen() const {
    if (closed_ || database_ == nullptr) {
      throw ManagerApplyJournalClosedError("manager Apply journal is closed");
    }
  }

  void Close() {
    auto startup_lock =
        sqlite_startup_internal::AcquireManagerSqliteStoreStartupLockForClose();
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    sqlite3_wal_checkpoint_v2(database_, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                              nullptr, nullptr);
    const auto result = sqlite3_close(database_);
    if (result != SQLITE_OK) {
      ThrowSqlite(database_, result, "close Apply-journal database");
    }
    database_ = nullptr;
    closed_ = true;
  }

  void Configure() {
    Exec(database_, "PRAGMA foreign_keys = ON");
    if (ScalarInteger(database_, "PRAGMA foreign_keys") != 1) {
      throw ManagerApplyJournalIntegrityError(
          "SQLite foreign-key enforcement is required");
    }
    Check(database_, sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds),
          "configure SQLite busy timeout");
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kBusyTimeoutMilliseconds);
    while (true) {
      try {
        Statement journal_mode(database_, "PRAGMA journal_mode = WAL");
        if (!journal_mode.Step() || journal_mode.Text(0) != "wal") {
          throw ManagerApplyJournalIntegrityError(
              "SQLite WAL journal mode is required");
        }
        break;
      } catch (const ManagerApplyJournalBusyError &) {
        if (std::chrono::steady_clock::now() >= deadline) {
          throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    Exec(database_, "PRAGMA synchronous = FULL");
    if (ScalarInteger(database_, "PRAGMA synchronous") != 2) {
      throw ManagerApplyJournalIntegrityError(
          "SQLite FULL synchronous mode is required");
    }
    Exec(database_, "PRAGMA trusted_schema = OFF");
    if (ScalarInteger(database_, "PRAGMA trusted_schema") != 0) {
      throw ManagerApplyJournalIntegrityError(
          "SQLite untrusted-schema mode is required");
    }
    Exec(database_, "PRAGMA recursive_triggers = OFF");
    if (ScalarInteger(database_, "PRAGMA recursive_triggers") != 0) {
      throw ManagerApplyJournalIntegrityError(
          "SQLite recursive triggers must remain disabled");
    }
    Exec(database_, "PRAGMA wal_autocheckpoint = 1000");
    if (ScalarInteger(database_, "PRAGMA wal_autocheckpoint") != 1000) {
      throw ManagerApplyJournalIntegrityError(
          "SQLite WAL autocheckpoint policy is required");
    }
    const auto journal_size_limit =
        ScalarInteger(database_, "PRAGMA journal_size_limit = " +
                                     std::to_string(kWalCheckpointTargetBytes));
    if (journal_size_limit != kWalCheckpointTargetBytes) {
      throw ManagerApplyJournalIntegrityError(
          "SQLite WAL journal-size policy is required");
    }
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    if (page_size <= 0) {
      throw ManagerApplyJournalIntegrityError("SQLite page size is invalid");
    }
    const auto maximum_pages = std::max<std::int64_t>(
        1, kMaximumManagerApplyJournalDatabasePageImageBytes / page_size);
    const auto page_count = ScalarInteger(database_, "PRAGMA page_count");
    if (page_count < 0 || page_count > maximum_pages) {
      throw ManagerApplyJournalLimitError(
          "SQLite main database page image exceeds its ceiling");
    }
    const auto effective_maximum_pages = ScalarInteger(
        database_, "PRAGMA max_page_count = " + std::to_string(maximum_pages));
    if (effective_maximum_pages != maximum_pages) {
      throw ManagerApplyJournalLimitError(
          "SQLite main database page ceiling could not be enforced");
    }
    sqlite3_limit(database_, SQLITE_LIMIT_LENGTH, 1024 * 1024);
  }

  void InitializeOrValidateSchema() {
    std::lock_guard lock(mutex_);
    Transaction transaction(database_, "BEGIN EXCLUSIVE");
    const auto existing = ReadSchemaObjects(database_);
    if (existing.empty()) {
      if (ScalarInteger(database_, "PRAGMA application_id") != 0 ||
          ScalarInteger(database_, "PRAGMA user_version") != 0) {
        throw ManagerApplyJournalIntegrityError(
            "schema-empty Apply-journal database has foreign SQLite "
            "identity");
      }
      Exec(database_, detail::kEmbeddedManagerApplyJournalSchema);
      Statement metadata(
          database_,
          "INSERT INTO apply_journal_metadata ("
          "singleton,schema_id,schema_version,schema_artifact_digest,cell_id,"
          "maximum_apply_intents,maximum_fence_attempts,"
          "maximum_affected_scopes,maximum_fence_components,"
          "maximum_fence_domains,maximum_fence_bindings,"
          "maximum_fence_resources,maximum_runtime_convergence_intents,"
          "maximum_runtime_staged_ready_sets,"
          "maximum_private_materializations,"
          "maximum_publication_receipts,"
          "maximum_fence_release_intents,maximum_fence_released_records,"
          "maximum_terminal_outcomes,"
          "maximum_main_database_page_image_bytes"
          ") VALUES (1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
      metadata.Bind(1, kManagerApplyJournalSchemaId);
      metadata.Bind(2, kManagerApplyJournalSchemaVersion);
      metadata.Bind(3, RawDigest(detail::kEmbeddedManagerApplyJournalSchema));
      metadata.Bind(4, cell_id_.value());
      metadata.Bind(5, static_cast<std::int64_t>(kMaximumManagerApplyIntents));
      metadata.Bind(
          6, static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts));
      metadata.Bind(
          7, static_cast<std::int64_t>(kMaximumManagerApplyAffectedScopes));
      metadata.Bind(
          8, static_cast<std::int64_t>(kMaximumManagerApplyFenceComponents));
      metadata.Bind(
          9, static_cast<std::int64_t>(kMaximumManagerApplyFenceDomains));
      metadata.Bind(
          10, static_cast<std::int64_t>(kMaximumManagerApplyFenceBindings));
      metadata.Bind(
          11, static_cast<std::int64_t>(kMaximumManagerApplyFenceResources));
      metadata.Bind(12, static_cast<std::int64_t>(kMaximumManagerApplyIntents));
      metadata.Bind(13,
                    static_cast<std::int64_t>(kMaximumRuntimeStagedReadySets));
      metadata.Bind(14, static_cast<std::int64_t>(
                            kMaximumManagerApplyPrivateMaterializations));
      metadata.Bind(15, static_cast<std::int64_t>(
                            kMaximumManagerApplyPublicationReceipts));
      metadata.Bind(16, static_cast<std::int64_t>(
                            kMaximumManagerApplyFenceReleaseIntents));
      metadata.Bind(17, static_cast<std::int64_t>(
                            kMaximumManagerApplyFenceReleasedRecords));
      metadata.Bind(
          18, static_cast<std::int64_t>(kMaximumManagerApplyTerminalOutcomes));
      metadata.Bind(19, kMaximumManagerApplyJournalDatabasePageImageBytes);
      static_cast<void>(metadata.Step());
    } else if (!existing.contains({"table", "apply_journal_metadata"})) {
      throw ManagerApplyJournalIntegrityError(
          "non-empty database lacks Apply-journal metadata");
    }
    ValidateMetadataLocked();
    if (ReadSchemaObjects(database_) != ExpectedSchemaObjects()) {
      throw ManagerApplyJournalIntegrityError(
          "SQLite schema objects differ from the versioned Apply artifact");
    }
    transaction.Commit();
  }

  void ValidateMetadataLocked() const {
    if (ScalarInteger(database_, "PRAGMA application_id") !=
        kManagerApplyJournalSqliteApplicationId) {
      throw ManagerApplyJournalIntegrityError(
          "Apply-journal SQLite application ID mismatch");
    }
    if (ScalarInteger(database_, "PRAGMA user_version") !=
        kManagerApplyJournalSchemaVersion) {
      throw ManagerApplyJournalIntegrityError(
          "Apply-journal SQLite user version mismatch");
    }
    ValidateApplyJournalMetadata(database_, cell_id_);
  }

  std::optional<std::int64_t>
  GenerationHighWaterLocked(const ApplyId &apply_id) const {
    Statement row(database_,
                  "SELECT apply_generation FROM manager_apply_intents "
                  "WHERE apply_id=? ORDER BY apply_generation DESC LIMIT 1");
    row.Bind(1, apply_id.value());
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto generation = row.Integer(0);
    if (generation <= 0 || row.Step()) {
      throw ManagerApplyJournalIntegrityError(
          "Apply generation high-water row is invalid");
    }
    return generation;
  }

  // Bound the journal by eviction instead of refusal. Delete, inside the
  // caller's write transaction, the oldest Apply (all generations of one
  // Apply ID, rows in every table) that no owner can still read:
  //  - it is terminal and holds no fence: none was accepted, its release is
  //    recorded, or its failed-Apply release was acknowledged;
  //  - its Kernel admission claim is provably settled, because the Kernel
  //    settlement and owner-reconciliation readers look the held claim's
  //    Apply up here: an acknowledged failed release; a success followed by a
  //    later directly admitted Apply (the Kernel admits none while a
  //    successful Apply's claim is held); a known failure that never accepted
  //    a fence (its claim was refused or released with the terminal record);
  //    or any other failure followed by such a settled success (its fresh
  //    settlement settles every older held claim). A Composer-parented Apply
  //    is reconciled from its terminal state, so it is settled when it
  //    succeeded or failed known at acceptance, or was acknowledged;
  //  - it is not the newest, newest terminal, newest successful or newest
  //    published Apply (current-product, settlement-retry and replay
  //    readers), and if newer than the newest success it accepted no fence
  //    (a failed candidate the host may still run is looked up by Apply).
  // Returns false when every retained Apply is still needed.
  bool EvictOneApplyLocked() {
    struct Row final {
      std::string apply_id;
      std::int64_t sequence{};
      bool terminal{}, succeeded{}, known_accepted{}, known_unfenced{};
      bool authoring{}, admitted{}, fence_held{}, acknowledged{};
      std::optional<std::int64_t> publication;
    };
    Statement select(
        database_,
        "SELECT i.apply_id,i.accepted_sequence,t.outcome_state,"
        "t.effect_disposition,t.failure_phase,"
        "i.parent_authoring_claim IS NOT NULL,"
        "EXISTS(SELECT 1 FROM manager_apply_affected_scopes x WHERE "
        "x.apply_id=i.apply_id AND x.apply_generation=i.apply_generation),"
        "EXISTS(SELECT 1 FROM manager_apply_fence_acceptances x WHERE "
        "x.apply_id=i.apply_id AND x.apply_generation=i.apply_generation),"
        "EXISTS(SELECT 1 FROM manager_apply_fence_released_records x WHERE "
        "x.apply_id=i.apply_id AND x.apply_generation=i.apply_generation),"
        "EXISTS(SELECT 1 FROM manager_apply_failed_release x WHERE "
        "x.apply_id=i.apply_id AND x.apply_generation=i.apply_generation "
        "AND x.acknowledged=1),"
        "(SELECT x.publication_generation FROM "
        "manager_apply_publication_receipts x WHERE "
        "x.apply_id=i.apply_id AND x.apply_generation=i.apply_generation) "
        "FROM manager_apply_intents i "
        "LEFT JOIN manager_apply_terminal_outcomes t "
        "ON t.apply_id=i.apply_id AND t.apply_generation=i.apply_generation "
        "ORDER BY i.accepted_sequence");
    std::vector<Row> rows;
    rows.reserve(kMaximumManagerApplyIntents);
    while (select.Step()) {
      Row row;
      row.apply_id = select.Text(0);
      row.sequence = select.Integer(1);
      row.terminal = !select.IsNull(2);
      row.succeeded = row.terminal && select.Text(2) == "succeeded";
      const bool known = !select.IsNull(3) && select.Text(3) == "known";
      row.known_accepted =
          known && !select.IsNull(4) && select.Text(4) == "accepted";
      row.authoring = select.Integer(5) != 0;
      row.admitted = select.Integer(6) != 0 && !row.authoring;
      const bool fenced = select.Integer(7) != 0;
      row.acknowledged = select.Integer(9) != 0;
      row.fence_held = fenced && select.Integer(8) == 0 && !row.acknowledged;
      row.known_unfenced = known && !fenced;
      if (!select.IsNull(10))
        row.publication = select.Integer(10);
      rows.push_back(std::move(row));
    }
    if (rows.empty())
      return false;

    std::int64_t newest_terminal = 0, newest_success = 0, latest_admitted = 0;
    std::int64_t newest_publication_sequence = 0, newest_publication = 0;
    for (const auto &row : rows) {
      if (row.terminal) newest_terminal = row.sequence;
      if (row.succeeded) newest_success = row.sequence;
      if (row.admitted) latest_admitted = row.sequence;
      if (row.publication && *row.publication > newest_publication) {
        newest_publication = *row.publication;
        newest_publication_sequence = row.sequence;
      }
    }
    // Rows are ordered by sequence: the nearest later success of each row.
    std::vector<std::int64_t> next_success(rows.size(), 0);
    for (std::size_t index = rows.size(); index-- > 1U;)
      next_success[index - 1U] =
          rows[index].succeeded ? rows[index].sequence : next_success[index];

    const auto evictable = [&](const Row &row, const std::int64_t later_success) {
      if (!row.terminal || row.fence_held ||
          row.sequence == rows.back().sequence ||
          row.sequence == newest_terminal || row.sequence == newest_success ||
          row.sequence == newest_publication_sequence)
        return false;
      if (row.sequence > newest_success && !row.known_unfenced)
        return false;
      if (row.acknowledged)
        return true;
      if (row.authoring)
        return row.succeeded || row.known_accepted;
      if (row.succeeded)
        return latest_admitted > row.sequence;
      if (row.known_unfenced)
        return true;
      return later_success != 0 && latest_admitted > later_success;
    };
    std::map<std::string, bool> candidates;
    std::vector<std::string> order;
    for (std::size_t index = 0U; index < rows.size(); ++index) {
      const auto [entry, inserted] =
          candidates.emplace(rows[index].apply_id, true);
      if (inserted)
        order.push_back(rows[index].apply_id);
      entry->second =
          entry->second && evictable(rows[index], next_success[index]);
    }
    const auto victim = std::find_if(order.begin(), order.end(),
                                     [&](const auto &id) { return candidates[id]; });
    if (victim == order.end())
      return false;
    // Children first: every foreign key points at a table deleted later.
    for (const char *table :
         {"manager_apply_failed_release", "manager_apply_terminal_outcomes",
          "manager_apply_fence_released_records",
          "manager_apply_fence_release_intents",
          "manager_apply_publication_receipts",
          "manager_apply_private_materializations",
          "manager_apply_runtime_staged_ready_sets",
          "manager_apply_runtime_convergence_intents",
          "manager_apply_fence_acceptances", "manager_apply_fence_attempts",
          "manager_apply_affected_scopes", "manager_apply_intents"}) {
      Statement remove(database_, std::string("DELETE FROM ") + table +
                                      " WHERE apply_id=?");
      remove.Bind(1, *victim);
      static_cast<void>(remove.Step());
    }
    return true;
  }

  ManagerApplyIntentWriteResult
  EnsureIntentValueLocked(const ManagerApplyIntentDTO &intent) {
    const auto encoded = EncodeIntent(intent);
    if (intent.cell_id != cell_id_) {
      throw ManagerApplyJournalConflictError(
          "Apply intent belongs to a different cell");
    }

    if (const auto existing =
            IntentLocked(intent.apply_id, encoded.apply_generation);
        existing.has_value()) {
      if (existing->intent != intent ||
          existing->intent_digest.value() != encoded.intent_digest) {
        throw ManagerApplyJournalConflictError(
            "Apply identity conflicts with the durable intent");
      }
      return {*existing, false};
    }
    if (const auto existing_request =
            RequestLocked(intent.request_context.request_owner_epoch,
                          intent.request_context.request_id);
        existing_request.has_value()) {
      throw ManagerApplyJournalConflictError(
          "public request identity already names another Apply intent");
    }

    const auto high_water = GenerationHighWaterLocked(intent.apply_id);
    if (high_water.has_value()) {
      const auto predecessor = IntentUncachedLocked(intent.apply_id, *high_water);
      ++record_verifications_;
      if (!predecessor)
        throw ManagerApplyJournalIntegrityError("Apply high-water intent missing");
    }
    if (high_water.has_value() &&
        *high_water == std::numeric_limits<std::int64_t>::max()) {
      throw ManagerApplyJournalLimitError(
          "Apply generation high-water mark is exhausted");
    }
    const auto expected_generation =
        high_water.has_value() ? *high_water + 1 : 1;
    if (encoded.apply_generation != expected_generation) {
      throw ManagerApplyJournalConflictError(
          "Apply generation must advance its durable high-water mark by "
          "exactly one");
    }
    const auto intent_count =
        ScalarInteger(database_, "SELECT COUNT(*) FROM manager_apply_intents");
    if (intent_count >=
            static_cast<std::int64_t>(kMaximumManagerApplyIntents) &&
        !EvictOneApplyLocked()) {
      throw ManagerApplyJournalLimitError(
          "Apply intent inventory reached its fixed limit");
    }
    // The newest Apply is never evicted, so this stays strictly increasing.
    const auto accepted_sequence = ScalarInteger(
        database_, "SELECT COALESCE(MAX(accepted_sequence),0)+1 "
                   "FROM manager_apply_intents");

    Statement insert(
        database_,
        "INSERT INTO manager_apply_intents ("
        "apply_id,apply_generation,request_id,request_owner_epoch,"
        "source_label,correlation_id,predecessor_retained_bundle_id,"
        "predecessor_graph_revision,predecessor_scene_id,"
        "predecessor_scene_revision_id,predecessor_scene_content_hash,"
        "candidate_retained_bundle_id,candidate_graph_revision,"
        "captured_input_digest,deployment_artifact_id,deployment_revision,"
        "program_binding_id,prepared_activation_id,"
        "prepared_activation_receipt_json,"
        "expected_admission_generation,parent_authoring_claim,"
        "intent_digest,scene_initialization,accepted_sequence"
        ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    insert.Bind(1, intent.apply_id.value());
    insert.Bind(2, encoded.apply_generation);
    insert.Bind(3, intent.request_context.request_id.value());
    insert.Bind(4, intent.request_context.request_owner_epoch.value());
    insert.Bind(5, intent.request_context.source_label);
    if (intent.request_context.correlation_id.has_value()) {
      insert.Bind(6, *intent.request_context.correlation_id);
    } else {
      insert.BindNull(6);
    }
    insert.Bind(
        7,
        intent.expected_active_projection_revision.retained_bundle_id.value());
    insert.Bind(
        8, intent.expected_active_projection_revision.graph_revision.value());
    insert.Bind(9, intent.expected_active_projection_revision.scene_revision
                       .scene_id.value());
    insert.Bind(10, encoded.predecessor_scene_revision_id);
    insert.Bind(11, intent.expected_active_projection_revision.scene_revision
                        .content_hash.value());
    insert.Bind(12, intent.candidate_retained_bundle_id.value());
    insert.Bind(13, intent.candidate_graph_revision.value());
    insert.Bind(14, intent.prepared_activation.captured_input_digest.value());
    insert.Bind(15, intent.prepared_activation.deployment_artifact_id.value());
    insert.Bind(16, intent.prepared_activation.deployment_revision.value());
    if (intent.prepared_activation.program_binding_id.has_value()) {
      insert.Bind(17, intent.prepared_activation.program_binding_id->value());
    } else {
      insert.BindNull(17);
    }
    insert.Bind(18, intent.prepared_activation.prepared_activation_id.value());
    insert.Bind(19, intent.prepared_activation.canonical_receipt_json);
    if (encoded.expected_admission_generation.has_value()) {
      insert.Bind(20, *encoded.expected_admission_generation);
    } else {
      insert.BindNull(20);
    }
    if (encoded.parent_authoring_claim.has_value()) {
      insert.BindBlob(21, *encoded.parent_authoring_claim);
    } else {
      insert.BindNull(21);
    }
    insert.Bind(22, encoded.intent_digest);
    insert.Bind(23, ToString(intent.scene_initialization));
    insert.Bind(24, accepted_sequence);
    static_cast<void>(insert.Step());

    const auto stored = IntentLocked(intent.apply_id, encoded.apply_generation);
    if (!stored.has_value() || stored->intent != intent ||
        stored->intent_digest.value() != encoded.intent_digest) {
      throw ManagerApplyJournalIntegrityError(
          "inserted Apply intent cannot be verified");
    }
    return {*stored, true};
  }

  using VerifiedReadKey = std::tuple<ApplyId, std::int64_t, std::int64_t>;
  template <typename Record>
  using VerifiedRecordMap = std::map<VerifiedReadKey, std::optional<Record>>;
  using VerifiedReads = std::tuple<
      VerifiedRecordMap<PersistedManagerApplyIntent>,
      VerifiedRecordMap<PersistedManagerApplyAffectedScope>,
      VerifiedRecordMap<PersistedManagerApplyFenceAttempt>,
      VerifiedRecordMap<PersistedManagerApplyFenceAcceptance>,
      VerifiedRecordMap<PersistedManagerApplyRuntimeConvergenceIntent>,
      VerifiedRecordMap<PersistedManagerApplyRuntimeStagedReady>,
      VerifiedRecordMap<PersistedManagerApplyPrivateMaterialization>,
      VerifiedRecordMap<PersistedManagerApplyPublicationReceipt>,
      VerifiedRecordMap<PersistedManagerApplyFenceReleaseIntent>,
      VerifiedRecordMap<PersistedManagerApplyFenceReleased>,
      VerifiedRecordMap<PersistedManagerApplyTerminalOutcome>>;

  // Generation predecessors are checked only during writes. Read the
  // immediate predecessor through the uncached decoder, without recursively
  // walking the generation history.
  class WriteVerificationScope final {
  public:
    explicit WriteVerificationScope(Impl &impl) noexcept : impl_(impl) {
      impl_.verify_write_generation_predecessors_ = true;
    }
    ~WriteVerificationScope() {
      impl_.verify_write_generation_predecessors_ = false;
    }
    WriteVerificationScope(const WriteVerificationScope &) = delete;
    WriteVerificationScope &operator=(const WriteVerificationScope &) = delete;

  private:
    Impl &impl_;
  };

  /// Installs the change-gated verified-record cache for one public read
  /// transaction. Must be called first inside that transaction: reading
  /// `data_version` opens its read snapshot. Any commit through another
  /// connection changes `data_version`; any write through this connection
  /// changes its total change count. Either discards every cached result, so
  /// a changed or corrupted record is re-read and re-verified by the very
  /// next read. A failed read discards the cache as well.
  class VerifiedReadScope final {
  public:
    explicit VerifiedReadScope(const Impl &impl) noexcept
        : impl_(impl), exceptions_(std::uncaught_exceptions()) {}
    ~VerifiedReadScope() {
      impl_.verified_reads_ = nullptr;
      if (std::uncaught_exceptions() != exceptions_) {
        impl_.change_gated_reads_.reset();
      }
    }
    VerifiedReadScope(const VerifiedReadScope &) = delete;
    VerifiedReadScope &operator=(const VerifiedReadScope &) = delete;

  private:
    const Impl &impl_;
    int exceptions_;
  };

  [[nodiscard]] VerifiedReadScope UseVerifiedReadsLocked() const {
    const auto data_version = ScalarInteger(database_, "PRAGMA data_version");
    const auto own_total_changes = sqlite3_total_changes64(database_);
    if (!change_gated_reads_.has_value() ||
        change_gated_reads_->data_version != data_version ||
        change_gated_reads_->own_total_changes != own_total_changes) {
      change_gated_reads_.emplace(
          ChangeGatedReads{data_version, own_total_changes, {}});
    }
    verified_reads_ = &change_gated_reads_->reads;
    return VerifiedReadScope(*this);
  }

  // Public reads reuse verified records while the database is provably
  // unchanged (see UseVerifiedReadsLocked). Writes and integrity verification
  // reuse verified predecessors only within their own locked operation; no
  // result survives that operation.
  template <typename Record, typename Reader>
  std::optional<Record> ReadVerifiedRecord(const VerifiedReadKey &key,
                                           Reader &&read) const {
    if (verified_reads_ == nullptr) {
      VerifiedReads reads;
      struct Reset final {
        VerifiedReads *&slot;
        ~Reset() { slot = nullptr; }
      } reset{verified_reads_};
      verified_reads_ = &reads;
      return ReadVerifiedRecord<Record>(key, std::forward<Reader>(read));
    }
    auto &records = std::get<VerifiedRecordMap<Record>>(*verified_reads_);
    if (const auto found = records.find(key); found != records.end()) {
      return found->second;
    }
    auto result = read();
    ++record_verifications_;
    records.emplace(key, result);
    return result;
  }

  std::optional<PersistedManagerApplyIntent>
  IntentLocked(const ApplyId &apply_id,
               const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyIntent>(
        {apply_id, apply_generation, 0}, [&] {
          auto result = IntentUncachedLocked(apply_id, apply_generation);
          if (result && verify_write_generation_predecessors_ &&
              apply_generation > 1) {
            const auto predecessor =
                IntentUncachedLocked(apply_id, apply_generation - 1);
            ++record_verifications_;
            if (!predecessor)
              throw ManagerApplyJournalIntegrityError(
                  "Apply predecessor intent missing");
          }
          return result;
        });
  }

  /// Compose one complete status view for exactly one Apply generation. Used by
  /// both the single-identity read and the bounded inventory read so the two
  /// surfaces cannot derive different membership.
  std::optional<PersistedManagerApplyStatus>
  StatusLocked(const ApplyId &apply_id,
               const std::int64_t apply_generation) const {
    auto intent = IntentLocked(apply_id, apply_generation);
    if (!intent.has_value()) {
      return std::nullopt;
    }
    return PersistedManagerApplyStatus{
        std::move(*intent),
        AffectedScopeLocked(apply_id, apply_generation),
        FenceAttemptLocked(apply_id, apply_generation),
        FenceAcceptanceLocked(apply_id, apply_generation),
        RuntimeConvergenceIntentLocked(apply_id, apply_generation),
        LatestRuntimeStagedReadyLocked(apply_id, apply_generation),
        PrivateMaterializationLocked(apply_id, apply_generation),
        PublicationReceiptLocked(apply_id, apply_generation),
        FenceReleaseIntentLocked(apply_id, apply_generation),
        FenceReleasedLocked(apply_id, apply_generation),
        TerminalOutcomeLocked(apply_id, apply_generation),
    };
  }

  std::optional<PersistedManagerApplyIntent>
  IntentUncachedLocked(const ApplyId &apply_id,
                       const std::int64_t apply_generation) const {
    Statement row(
        database_,
        "SELECT apply_id,apply_generation,request_id,request_owner_epoch,"
        "source_label,correlation_id,predecessor_retained_bundle_id,"
        "predecessor_graph_revision,predecessor_scene_id,"
        "predecessor_scene_revision_id,predecessor_scene_content_hash,"
        "candidate_retained_bundle_id,candidate_graph_revision,"
        "captured_input_digest,deployment_artifact_id,deployment_revision,"
        "program_binding_id,prepared_activation_id,"
        "prepared_activation_receipt_json,"
        "expected_admission_generation,parent_authoring_claim,"
        "intent_digest,scene_initialization "
        "FROM manager_apply_intents "
        "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto stored_apply_id = row.Text(0);
    const auto stored_generation = row.Integer(1);
    const auto request_id = row.Text(2);
    const auto request_owner_epoch = row.Text(3);
    const auto source_label = row.Text(4);
    const auto correlation_id = row.OptionalText(5);
    const auto predecessor_retained_bundle_id = row.Text(6);
    const auto predecessor_graph_revision = row.Text(7);
    const auto predecessor_scene_id = row.Text(8);
    const auto predecessor_scene_revision_id = row.Integer(9);
    const auto predecessor_scene_content_hash = row.Text(10);
    const auto candidate_retained_bundle_id = row.Text(11);
    const auto candidate_graph_revision = row.Text(12);
    const auto captured_input_digest = row.Text(13);
    const auto deployment_artifact_id = row.Text(14);
    const auto deployment_revision = row.Text(15);
    const auto program_binding_id = row.OptionalText(16);
    const auto prepared_activation_id = row.Text(17);
    const auto prepared_activation_receipt_json = row.Text(18);
    std::optional<core::AdmissionGeneration> expected_admission_generation;
    if (!row.IsNull(19)) {
      const auto stored_admission_generation = row.Integer(19);
      if (stored_admission_generation <= 0) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply admission generation is invalid");
      }
      expected_admission_generation.emplace(
          static_cast<std::uint64_t>(stored_admission_generation));
    }
    const auto parent_authoring_claim = row.OptionalBlob(20);
    const auto stored_digest = row.Text(21);
    const auto scene_initialization = row.Text(22);
    if (scene_initialization != "preserve" &&
        scene_initialization != "initialize") {
      throw ManagerApplyJournalIntegrityError(
          "stored Scene initialization purpose is invalid");
    }
    if (row.Step()) {
      throw ManagerApplyJournalIntegrityError(
          "Apply intent identity is not unique");
    }
    if (stored_generation <= 0 || predecessor_scene_revision_id <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply intent generation is invalid");
    }

    ManagerApplyIntentDTO intent{
        PublicRequestContextDTO{
            RequestId(request_id),
            RequestOwnerEpoch(request_owner_epoch),
            source_label,
            correlation_id,
        },
        ApplyId(stored_apply_id),
        ApplyGeneration(static_cast<std::uint64_t>(stored_generation)),
        cell_id_,
        ProjectionRevisionDTO{
            RetainedBundleId(predecessor_retained_bundle_id),
            GraphRevision(predecessor_graph_revision),
            SceneRevision{
                SceneId(predecessor_scene_id),
                static_cast<std::uint64_t>(predecessor_scene_revision_id),
                ContentDigest(predecessor_scene_content_hash),
            },
        },
        RetainedBundleId(candidate_retained_bundle_id),
        GraphRevision(candidate_graph_revision),
        ManagerApplyPreparedActivationDTO{
            ContentDigest(captured_input_digest),
            core::DeploymentArtifactId(deployment_artifact_id),
            core::DeploymentRevision(deployment_revision),
            program_binding_id.has_value()
                ? std::optional(core::ProgramBindingId(*program_binding_id))
                : std::nullopt,
            ContentDigest(prepared_activation_id),
            prepared_activation_receipt_json,
        },
        expected_admission_generation,
        parent_authoring_claim.has_value()
            ? std::optional<core::CellAdmissionClaimDTO>(
                  DecodeParentAuthoringClaim(*parent_authoring_claim))
            : std::nullopt,
        scene_initialization == "initialize"
            ? ManagerSceneInitialization::kInitialize
            : ManagerSceneInitialization::kPreserve,
    };
    EncodedIntent encoded;
    try {
      encoded = EncodeIntent(intent);
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError("stored Apply intent is invalid");
    }
    if (encoded.intent_digest != stored_digest) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply intent digest is inconsistent");
    }
    return PersistedManagerApplyIntent{std::move(intent),
                                       ContentDigest(stored_digest)};
  }

  std::optional<PersistedManagerApplyIntent>
  RequestLocked(const RequestOwnerEpoch &request_owner_epoch,
                const RequestId &request_id) const {
    Statement row(database_,
                  "SELECT apply_id,apply_generation FROM manager_apply_intents "
                  "WHERE request_owner_epoch=? AND request_id=?");
    row.Bind(1, request_owner_epoch.value());
    row.Bind(2, request_id.value());
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto apply_id = ApplyId(row.Text(0));
    const auto generation = row.Integer(1);
    if (generation <= 0 || row.Step()) {
      throw ManagerApplyJournalIntegrityError(
          "Apply request identity is not unique");
    }
    const auto intent = IntentLocked(apply_id, generation);
    if (!intent.has_value()) {
      throw ManagerApplyJournalIntegrityError(
          "Apply request identity references a missing intent");
    }
    return intent;
  }

  std::optional<PersistedManagerApplyAffectedScope>
  AffectedScopeLocked(const ApplyId &apply_id,
                      const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyAffectedScope>(
        {apply_id, apply_generation, 0}, [&] {
          return AffectedScopeUncachedLocked(apply_id, apply_generation);
        });
  }

  std::optional<PersistedManagerApplyAffectedScope>
  AffectedScopeUncachedLocked(const ApplyId &apply_id,
                              const std::int64_t apply_generation) const {
    Statement row(database_,
                  "SELECT intent_digest,components_encoding,domains_encoding,"
                  "bindings_encoding,resources_encoding,busy_disposition,"
                  "affected_scope_digest,component_delta_encoding "
                  "FROM manager_apply_affected_scopes "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto intent_digest = row.Text(0);
    const auto components = row.Text(1);
    const auto domains = row.Text(2);
    const auto bindings = row.Text(3);
    const auto resources = row.Text(4);
    const auto busy_disposition = row.Text(5);
    const auto stored_digest = row.Text(6);
    const auto component_delta = row.Text(7);
    if (row.Step()) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply affected scope is not unique");
    }
    try {
      ManagerApplyAffectedScopeDTO affected_scope{
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          DecodeScope(components, domains, bindings, resources),
          DecodeBusyDisposition(busy_disposition),
          manyforge::deployment::DecodeApplyComponentDelta(component_delta),
      };
      const ContentDigest parent_digest(intent_digest);
      const auto encoded = EncodeAffectedScope(parent_digest, affected_scope);
      if (encoded.digest != stored_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply affected-scope digest is inconsistent");
      }
      const auto parent = IntentLocked(apply_id, apply_generation);
      if (!parent.has_value() || parent->intent_digest != parent_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply affected scope is not bound to its exact intent");
      }
      return PersistedManagerApplyAffectedScope{
          std::move(affected_scope),
          parent_digest,
          ContentDigest(stored_digest),
      };
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply affected scope contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplyFenceAttempt>
  FenceAttemptLocked(const ApplyId &apply_id,
                     const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyFenceAttempt>(
        {apply_id, apply_generation, 0},
        [&] { return FenceAttemptUncachedLocked(apply_id, apply_generation); });
  }

  std::optional<PersistedManagerApplyFenceAttempt>
  FenceAttemptUncachedLocked(const ApplyId &apply_id,
                             const std::int64_t apply_generation) const {
    Statement row(
        database_,
        "SELECT intent_digest,affected_scope_digest,request_id,"
        "request_owner_epoch,source_label,correlation_id,components_encoding,"
        "domains_encoding,bindings_encoding,resources_encoding,"
        "busy_disposition,expected_retained_bundle_id,"
        "expected_graph_revision,expected_operation_resource_generation,"
        "expected_fence_generation,active_configurations_encoding,reason,"
        "fence_attempt_digest FROM manager_apply_fence_attempts "
        "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto intent_digest = row.Text(0);
    const auto affected_scope_digest = row.Text(1);
    const auto request_id = row.Text(2);
    const auto owner_epoch = row.Text(3);
    const auto source_label = row.Text(4);
    const auto correlation_id = row.OptionalText(5);
    const auto components = row.Text(6);
    const auto domains = row.Text(7);
    const auto bindings = row.Text(8);
    const auto resources = row.Text(9);
    const auto busy_disposition = row.Text(10);
    const auto retained_bundle = row.Text(11);
    const auto graph_revision = row.Text(12);
    const auto operation_generation = row.Integer(13);
    const auto fence_generation = row.Integer(14);
    const auto active_configurations = row.Text(15);
    const auto reason = row.Text(16);
    const auto stored_digest = row.Text(17);
    if (row.Step() || operation_generation < 0 || fence_generation < 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-fence attempt is structurally invalid");
    }
    try {
      const auto configurations =
          DecodeActiveConfigurations(active_configurations);
      ApplyConfigurationFenceIntentDTO intent{
          PublicRequestContextDTO{
              RequestId(request_id),
              RequestOwnerEpoch(owner_epoch),
              source_label,
              correlation_id,
          },
          core::ApplyFenceOwnerDTO{
              apply_id,
              ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          },
          DecodeScope(components, domains, bindings, resources),
          DecodeBusyDisposition(busy_disposition),
          RetainedBundleId(retained_bundle),
          GraphRevision(graph_revision),
          static_cast<std::uint64_t>(operation_generation),
          ConfigurationFenceGeneration(
              static_cast<std::uint64_t>(fence_generation)),
          configurations,
          reason,
      };
      const ContentDigest parent_digest(intent_digest);
      const ContentDigest scope_digest(affected_scope_digest);
      const auto encoded =
          EncodeFenceAttempt(parent_digest, scope_digest, intent);
      if (encoded.digest != stored_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-fence attempt digest is inconsistent");
      }
      const auto parent = IntentLocked(apply_id, apply_generation);
      const auto affected_scope =
          AffectedScopeLocked(apply_id, apply_generation);
      if (!parent.has_value() || parent->intent_digest != parent_digest ||
          !affected_scope.has_value() ||
          affected_scope->affected_scope_digest != scope_digest ||
          affected_scope->affected_scope.scope != intent.scope ||
          affected_scope->affected_scope.busy_disposition !=
              intent.busy_disposition ||
          parent->intent.request_context != intent.owner_request_context ||
          parent->intent.expected_active_projection_revision
                  .retained_bundle_id != intent.expected_retained_bundle_id ||
          parent->intent.expected_active_projection_revision.graph_revision !=
              intent.expected_graph_revision) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-fence attempt is not bound to its exact intent");
      }
      return PersistedManagerApplyFenceAttempt{
          std::move(intent),
          parent_digest,
          scope_digest,
          ContentDigest(stored_digest),
      };
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-fence attempt contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplyFenceAcceptance>
  FenceAcceptanceLocked(const ApplyId &apply_id,
                        const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyFenceAcceptance>(
        {apply_id, apply_generation, 0}, [&] {
          return FenceAcceptanceUncachedLocked(apply_id, apply_generation);
        });
  }

  std::optional<PersistedManagerApplyFenceAcceptance>
  FenceAcceptanceUncachedLocked(const ApplyId &apply_id,
                                const std::int64_t apply_generation) const {
    Statement row(database_,
                  "SELECT fence_attempt_digest,configuration_fence_id,"
                  "configuration_fence_generation,captured_components_encoding,"
                  "captured_domains_encoding,captured_bindings_encoding,"
                  "captured_resources_encoding,accepted_at_ns,replay_status,"
                  "acceptance_digest FROM manager_apply_fence_acceptances "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto attempt_digest = row.Text(0);
    const auto fence_id = row.Text(1);
    const auto fence_generation = row.Integer(2);
    const auto components = row.Text(3);
    const auto domains = row.Text(4);
    const auto bindings = row.Text(5);
    const auto resources = row.Text(6);
    const auto accepted_at_ns = row.Integer(7);
    const auto replay_status = row.Text(8);
    const auto stored_digest = row.Text(9);
    if (row.Step() || fence_generation <= 0 || accepted_at_ns < 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-fence acceptance is structurally invalid");
    }
    try {
      core::RequestReplayStatusDTO status;
      if (replay_status == "accepted_new") {
        status = core::RequestReplayStatusDTO::kAcceptedNew;
      } else if (replay_status == "replayed_exact") {
        status = core::RequestReplayStatusDTO::kReplayedExact;
      } else {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-fence replay status is invalid");
      }
      ConfigurationFenceAcceptedDTO accepted{
          ConfigurationFenceId(fence_id),
          ConfigurationFenceGeneration(
              static_cast<std::uint64_t>(fence_generation)),
          DecodeScope(components, domains, bindings, resources),
          accepted_at_ns,
          status,
      };
      const ContentDigest parent_digest(attempt_digest);
      const auto encoded = EncodeFenceAcceptance(
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          parent_digest, accepted);
      if (encoded.digest != stored_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-fence acceptance digest is inconsistent");
      }
      const auto attempt = FenceAttemptLocked(apply_id, apply_generation);
      if (!attempt.has_value() ||
          attempt->fence_attempt_digest != parent_digest ||
          !accepted.matches(attempt->fence_intent)) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-fence acceptance is not bound to its exact attempt");
      }
      return PersistedManagerApplyFenceAcceptance{
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          parent_digest,
          std::move(accepted),
          ContentDigest(stored_digest),
      };
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-fence acceptance contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplyRuntimeConvergenceIntent>
  RuntimeConvergenceIntentLocked(const ApplyId &apply_id,
                                 const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyRuntimeConvergenceIntent>(
        {apply_id, apply_generation, 0}, [&] {
          return RuntimeConvergenceIntentUncachedLocked(apply_id,
                                                        apply_generation);
        });
  }

  std::optional<PersistedManagerApplyRuntimeConvergenceIntent>
  RuntimeConvergenceIntentUncachedLocked(
      const ApplyId &apply_id, const std::int64_t apply_generation) const {
    Statement row(database_,
                  "SELECT intent_digest,candidate_retained_bundle_id,"
                  "candidate_graph_revision,fence_acceptance_digest,"
                  "predecessor_publication_generation,"
                  "predecessor_public_pointer_digest,runtime_units_encoding,"
                  "runtime_convergence_intent_digest "
                  "FROM manager_apply_runtime_convergence_intents "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto intent_digest = row.Text(0);
    const auto candidate_bundle = row.Text(1);
    const auto candidate_graph = row.Text(2);
    const auto acceptance_digest = row.Text(3);
    const auto publication_generation = row.Integer(4);
    const auto public_pointer_digest = row.Text(5);
    const auto runtime_units = row.Text(6);
    const auto stored_digest = row.Text(7);
    if (row.Step() || publication_generation <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored runtime convergence intent is structurally invalid");
    }
    try {
      ManagerApplyRuntimeConvergenceIntentDTO intent{
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          RetainedBundleId(candidate_bundle),
          GraphRevision(candidate_graph),
          ContentDigest(acceptance_digest),
          core::ActiveBundlePublicationGeneration(
              static_cast<std::uint64_t>(publication_generation)),
          ContentDigest(public_pointer_digest),
          DecodeRuntimePlanRows(runtime_units),
      };
      const ContentDigest parent_digest(intent_digest);
      const auto encoded =
          EncodeRuntimeConvergenceIntent(parent_digest, intent);
      const auto parent = IntentLocked(apply_id, apply_generation);
      const auto acceptance = FenceAcceptanceLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !parent.has_value() ||
          parent->intent_digest != parent_digest ||
          parent->intent.candidate_retained_bundle_id !=
              intent.candidate_retained_bundle_id ||
          parent->intent.candidate_graph_revision !=
              intent.candidate_graph_revision ||
          !acceptance.has_value() ||
          acceptance->acceptance_digest != intent.fence_acceptance_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored runtime convergence intent is not bound to its exact "
            "Apply/fence records");
      }
      return PersistedManagerApplyRuntimeConvergenceIntent{
          std::move(intent), parent_digest, ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored runtime convergence intent contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplyRuntimeStagedReady>
  RuntimeStagedReadyLocked(const ApplyId &apply_id,
                           const std::int64_t apply_generation,
                           const std::int64_t staged_ready_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyRuntimeStagedReady>(
        {apply_id, apply_generation, staged_ready_generation}, [&] {
          if (verify_write_generation_predecessors_) {
            Statement count(database_,
                "SELECT COUNT(*) FROM manager_apply_runtime_staged_ready_sets "
                "WHERE apply_id=? AND apply_generation=?");
            count.Bind(1, apply_id.value());
            count.Bind(2, apply_generation);
            if (!count.Step() || count.Integer(0) > static_cast<std::int64_t>(
                                      kMaximumRuntimeStagedReadySetsPerApply))
              throw ManagerApplyJournalIntegrityError(
                  "stored runtime staged-ready inventory exceeds its ceiling");
          }
          auto result = RuntimeStagedReadyUncachedLocked(
              apply_id, apply_generation, staged_ready_generation);
          if (result && verify_write_generation_predecessors_ &&
              staged_ready_generation > static_cast<std::int64_t>(
                                            kMaximumRuntimeStagedReadySetsPerApply))
            throw ManagerApplyJournalIntegrityError(
                "stored runtime staged-ready generation exceeds its ceiling");
          if (result && verify_write_generation_predecessors_ &&
              staged_ready_generation > 1) {
            const auto predecessor = RuntimeStagedReadyUncachedLocked(
                apply_id, apply_generation, staged_ready_generation - 1);
            ++record_verifications_;
            if (!predecessor)
              throw ManagerApplyJournalIntegrityError(
                  "staged-ready predecessor record missing");
          }
          return result;
        });
  }

  std::optional<PersistedManagerApplyRuntimeStagedReady>
  RuntimeStagedReadyUncachedLocked(
      const ApplyId &apply_id, const std::int64_t apply_generation,
      const std::int64_t staged_ready_generation) const {
    Statement row(database_,
                  "SELECT runtime_convergence_intent_digest,"
                  "predecessor_publication_generation,"
                  "predecessor_public_pointer_digest,evaluated_at_ns,"
                  "runtime_evidence_encoding,staged_ready_digest "
                  "FROM manager_apply_runtime_staged_ready_sets "
                  "WHERE apply_id=? AND apply_generation=? "
                  "AND staged_ready_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    row.Bind(3, staged_ready_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto convergence_digest = row.Text(0);
    const auto publication_generation = row.Integer(1);
    const auto public_pointer_digest = row.Text(2);
    const auto evaluated_at_ns = row.Integer(3);
    const auto runtime_evidence = row.Text(4);
    const auto stored_digest = row.Text(5);
    if (row.Step() || staged_ready_generation <= 0 ||
        publication_generation <= 0 || evaluated_at_ns <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored runtime staged-ready set is structurally invalid");
    }
    try {
      ManagerApplyRuntimeStagedReadyDTO staged_ready{
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          static_cast<std::uint64_t>(staged_ready_generation),
          ContentDigest(convergence_digest),
          core::ActiveBundlePublicationGeneration(
              static_cast<std::uint64_t>(publication_generation)),
          ContentDigest(public_pointer_digest),
          static_cast<std::uint64_t>(evaluated_at_ns),
          DecodeRuntimeStagedReadyRows(runtime_evidence),
      };
      const auto encoded = EncodeRuntimeStagedReady(staged_ready);
      const auto intent =
          RuntimeConvergenceIntentLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !intent.has_value() ||
          intent->runtime_convergence_intent_digest !=
              staged_ready.runtime_convergence_intent_digest ||
          intent->runtime_convergence_intent
                  .predecessor_publication_generation !=
              staged_ready.predecessor_publication_generation ||
          intent->runtime_convergence_intent
                  .predecessor_public_pointer_digest !=
              staged_ready.predecessor_public_pointer_digest ||
          intent->runtime_convergence_intent.runtime_units.size() !=
              staged_ready.runtime_units.size()) {
        throw ManagerApplyJournalIntegrityError(
            "stored runtime staged-ready set is not bound to its exact "
            "convergence intent");
      }
      for (std::size_t index = 0U; index < staged_ready.runtime_units.size();
           ++index) {
        const auto &plan =
            intent->runtime_convergence_intent.runtime_units[index];
        const auto &evidence = staged_ready.runtime_units[index];
        if (evidence.runtime_unit_id != plan.runtime_unit_id ||
            evidence.lifecycle_control != plan.lifecycle_control ||
            evidence.candidate_disposition != plan.candidate_disposition ||
            evidence.observed_product_identity != plan.product_identity ||
            evidence.observed_configuration_identity !=
                plan.configuration_identity ||
            evidence.observed_runtime_contract_offer_digest !=
                plan.runtime_contract_offer_digest ||
            evidence.ready_runtime_role_ids != plan.required_runtime_role_ids ||
            evidence.dependency_runtime_epochs.size() !=
                plan.dependency_runtime_unit_ids.size()) {
          throw ManagerApplyJournalIntegrityError(
              "stored runtime staged-ready row differs from its plan");
        }
        for (std::size_t dependency = 0U;
             dependency < evidence.dependency_runtime_epochs.size();
             ++dependency) {
          if (evidence.dependency_runtime_epochs[dependency].runtime_unit_id !=
              plan.dependency_runtime_unit_ids[dependency]) {
            throw ManagerApplyJournalIntegrityError(
                "stored runtime staged-ready dependency differs from its "
                "plan");
          }
        }
      }
      return PersistedManagerApplyRuntimeStagedReady{
          std::move(staged_ready), ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored runtime staged-ready set contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplyRuntimeStagedReady>
  LatestRuntimeStagedReadyLocked(const ApplyId &apply_id,
                                 const std::int64_t apply_generation,
                                 const bool predecessor_only = false) const {
    Statement row(database_, "SELECT staged_ready_generation "
                             "FROM manager_apply_runtime_staged_ready_sets "
                             "WHERE apply_id=? AND apply_generation=? "
                             "ORDER BY staged_ready_generation DESC LIMIT 1");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto generation = row.Integer(0);
    if (generation <= 0 || row.Step()) {
      throw ManagerApplyJournalIntegrityError(
          "runtime staged-ready high-water row is invalid");
    }
    if (predecessor_only) {
      if (generation > static_cast<std::int64_t>(
                           kMaximumRuntimeStagedReadySetsPerApply))
        throw ManagerApplyJournalIntegrityError(
            "stored runtime staged-ready generation exceeds its ceiling");
      auto result = RuntimeStagedReadyUncachedLocked(apply_id, apply_generation,
                                                      generation);
      ++record_verifications_;
      return result;
    }
    return RuntimeStagedReadyLocked(apply_id, apply_generation, generation);
  }

  // Scene-stage intent records the exact staged-ready digest that justified
  // the irreversible scene transition.  Runtime recovery may append a newer
  // staged-ready observation (for example after the Manager recreates a
  // native child with a new process epoch), so looking up the scene evidence
  // by the high-water generation would incorrectly make the immutable stage
  // intent appear corrupt.
  std::optional<PersistedManagerApplyRuntimeStagedReady>
  RuntimeStagedReadyByDigestLocked(const ApplyId &apply_id,
                                   const std::int64_t apply_generation,
                                   const ContentDigest &staged_ready_digest) const {
    Statement rows(
        database_,
        "SELECT staged_ready_generation "
        "FROM manager_apply_runtime_staged_ready_sets "
        "WHERE apply_id=? AND apply_generation=? AND staged_ready_digest=? "
        "ORDER BY staged_ready_generation");
    rows.Bind(1, apply_id.value());
    rows.Bind(2, apply_generation);
    rows.Bind(3, staged_ready_digest.value());
    if (!rows.Step()) {
      return std::nullopt;
    }
    const auto generation = rows.Integer(0);
    if (generation <= 0 || rows.Step()) {
      throw ManagerApplyJournalIntegrityError(
          "runtime staged-ready digest does not identify exactly one row");
    }
    return RuntimeStagedReadyLocked(apply_id, apply_generation, generation);
  }

  std::optional<PersistedManagerApplyPrivateMaterialization>
  PrivateMaterializationLocked(const ApplyId &apply_id,
                               const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyPrivateMaterialization>(
        {apply_id, apply_generation, 0}, [&] {
          return PrivateMaterializationUncachedLocked(apply_id,
                                                      apply_generation);
        });
  }

  std::optional<PersistedManagerApplyPrivateMaterialization>
  PrivateMaterializationUncachedLocked(
      const ApplyId &apply_id, const std::int64_t apply_generation) const {
    Statement row(database_,
                  "SELECT authoritative_scene_snapshot_digest,"
                  "candidate_retained_bundle_id,candidate_graph_revision,"
                  "candidate_scene_id,candidate_scene_revision_id,"
                  "candidate_scene_content_hash,"
                  "retained_bundle_canonical_content_digest,"
                  "retained_bundle_canonical_byte_size,"
                  "private_materialization_content_digest "
                  "FROM manager_apply_private_materializations "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto snapshot_digest = row.Text(0);
    const auto candidate_bundle = row.Text(1);
    const auto candidate_graph = row.Text(2);
    const auto candidate_scene_id = row.Text(3);
    const auto candidate_scene_revision = row.Integer(4);
    const auto candidate_scene_digest = row.Text(5);
    const auto canonical_digest = row.Text(6);
    const auto canonical_size = row.Integer(7);
    const auto stored_digest = row.Text(8);
    if (row.Step() || apply_generation <= 0 ||
        candidate_scene_revision <= 0 || canonical_size <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored private materialization is structurally invalid");
    }
    try {
      ManagerApplyPrivateMaterializationDTO materialization{
          ContentDigest(snapshot_digest),
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          ProjectionRevisionDTO{
              RetainedBundleId(candidate_bundle),
              GraphRevision(candidate_graph),
              SceneRevision{
                  SceneId(candidate_scene_id),
                  static_cast<std::uint64_t>(candidate_scene_revision),
                  ContentDigest(candidate_scene_digest),
              },
          },
          ContentDigest(canonical_digest),
          static_cast<std::uint64_t>(canonical_size),
      };
      const auto encoded = EncodePrivateMaterialization(materialization);
      const auto intent = IntentLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !intent.has_value() ||
          materialization.authoritative_scene_snapshot_digest !=
              materialization.candidate_projection_revision.scene_revision
                  .content_hash ||
          intent->intent.candidate_retained_bundle_id !=
              materialization.candidate_projection_revision
                  .retained_bundle_id ||
          intent->intent.candidate_graph_revision !=
              materialization.candidate_projection_revision.graph_revision) {
        throw ManagerApplyJournalIntegrityError(
            "stored private materialization is not bound to its exact Apply "
            "and authoritative Scene snapshot");
      }
      return PersistedManagerApplyPrivateMaterialization{
          std::move(materialization), ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored private materialization contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplyPublicationReceipt>
  PublicationReceiptLocked(const ApplyId &apply_id,
                           const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyPublicationReceipt>(
        {apply_id, apply_generation, 0}, [&] {
          return PublicationReceiptUncachedLocked(apply_id, apply_generation);
        });
  }

  std::optional<PersistedManagerApplyPublicationReceipt>
  PublicationReceiptUncachedLocked(const ApplyId &apply_id,
                                   const std::int64_t apply_generation) const {
    Statement row(
        database_,
        "SELECT candidate_retained_bundle_id,"
        "candidate_graph_revision,candidate_scene_id,"
        "candidate_scene_revision_id,candidate_scene_content_hash,"
        "expected_previous_publication_generation,"
        "private_materialization_content_digest,publication_generation,"
        "durable_public_pointer_content_digest,publication_receipt_digest "
        "FROM manager_apply_publication_receipts "
        "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto candidate_bundle = row.Text(0);
    const auto candidate_graph = row.Text(1);
    const auto candidate_scene_id = row.Text(2);
    const auto candidate_scene_revision = row.Integer(3);
    const auto candidate_scene_digest = row.Text(4);
    const auto previous_generation = row.Integer(5);
    const auto materialization_digest = row.Text(6);
    const auto publication_generation = row.Integer(7);
    const auto public_pointer_digest = row.Text(8);
    const auto stored_digest = row.Text(9);
    if (row.Step() || apply_generation <= 0 ||
        candidate_scene_revision <= 0 || previous_generation <= 0 ||
        previous_generation == std::numeric_limits<std::int64_t>::max() ||
        publication_generation <= 0 ||
        publication_generation != previous_generation + 1) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply publication receipt is structurally invalid");
    }
    try {
      const ProjectionRevisionDTO candidate{
          RetainedBundleId(candidate_bundle),
          GraphRevision(candidate_graph),
          SceneRevision{
              SceneId(candidate_scene_id),
              static_cast<std::uint64_t>(candidate_scene_revision),
              ContentDigest(candidate_scene_digest),
          },
      };
      ManagerApplyPublicationReceiptDTO publication{
          ContentDigest(materialization_digest),
          ActiveBundlePublicationReceiptDTO{
              ActiveBundlePublicationRequestDTO{
                  NamedApplyPublicationTransactionDTO{
                      apply_id,
                      ApplyGeneration(
                          static_cast<std::uint64_t>(apply_generation)),
                  },
                  candidate,
                  candidate.retained_bundle_id,
                  candidate.graph_revision,
                  ActiveBundlePublicationGeneration(
                      static_cast<std::uint64_t>(previous_generation)),
                  ContentDigest(materialization_digest),
              },
              ActiveBundlePublicationGeneration(
                  static_cast<std::uint64_t>(publication_generation)),
              ContentDigest(public_pointer_digest),
          },
      };
      const auto encoded = EncodePublicationReceipt(publication);
      const auto materialization =
          PrivateMaterializationLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !materialization.has_value() ||
          materialization->private_materialization_content_digest !=
              publication.private_materialization_content_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply publication is not bound to its exact predecessor "
            "records");
      }
      return PersistedManagerApplyPublicationReceipt{
          std::move(publication), ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply publication contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplyFenceReleaseIntent>
  FenceReleaseIntentLocked(const ApplyId &apply_id,
                           const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyFenceReleaseIntent>(
        {apply_id, apply_generation, 0}, [&] {
          return FenceReleaseIntentUncachedLocked(apply_id, apply_generation);
        });
  }

  std::optional<PersistedManagerApplyFenceReleaseIntent>
  FenceReleaseIntentUncachedLocked(const ApplyId &apply_id,
                                   const std::int64_t apply_generation) const {
    Statement row(database_,
                  "SELECT scene_completion_digest,fence_acceptance_digest,"
                  "release_request_encoding,fence_release_intent_digest "
                  "FROM manager_apply_fence_release_intents "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto scene_completion_digest = row.Text(0);
    const auto fence_acceptance_digest = row.Text(1);
    const auto request_encoding = row.Text(2);
    const auto stored_digest = row.Text(3);
    if (row.Step() || apply_generation <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored fence-release intent is structurally invalid");
    }
    try {
      ManagerApplyFenceReleaseIntentDTO intent{
          ContentDigest(scene_completion_digest),
          ContentDigest(fence_acceptance_digest),
          DecodeFenceReleaseRequest(request_encoding),
      };
      const auto encoded = EncodeFenceReleaseIntent(intent);
      const auto &evidence = intent.release_request.reconciliation_evidence;
      const auto acceptance = FenceAcceptanceLocked(apply_id, apply_generation);
      const auto publication =
          PublicationReceiptLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest ||
          evidence.owner_transaction.apply_id != apply_id ||
          evidence.owner_transaction.apply_generation.value() !=
              static_cast<std::uint64_t>(apply_generation) ||
          !publication.has_value() ||
          publication->publication_receipt_digest !=
              intent.scene_completion_digest ||
          !acceptance.has_value() ||
          acceptance->acceptance_digest != intent.fence_acceptance_digest ||
          acceptance->accepted_fence.configuration_fence_id !=
              evidence.configuration_fence_id ||
          acceptance->accepted_fence.configuration_fence_generation !=
              evidence.configuration_fence_generation ||
          acceptance->accepted_fence.captured_scope !=
              evidence.reconciled_scope ||
          publication->publication.publication_receipt.request
                  .candidate_projection_revision !=
              evidence.active_projection_revision ||
          publication->publication.publication_receipt.publication_generation !=
              evidence.active_publication_generation ||
          publication->publication.publication_receipt
                  .durable_public_pointer_content_digest !=
              evidence.active_public_pointer_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored fence-release intent is not bound to exact completed, "
            "fence, and publication evidence");
      }
      return PersistedManagerApplyFenceReleaseIntent{
          std::move(intent), ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored fence-release intent contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplyFenceReleased>
  FenceReleasedLocked(const ApplyId &apply_id,
                      const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyFenceReleased>(
        {apply_id, apply_generation, 0}, [&] {
          return FenceReleasedUncachedLocked(apply_id, apply_generation);
        });
  }

  std::optional<PersistedManagerApplyFenceReleased>
  FenceReleasedUncachedLocked(const ApplyId &apply_id,
                              const std::int64_t apply_generation) const {
    Statement row(database_, "SELECT fence_release_intent_digest,"
                             "released_fence_encoding,fence_released_digest "
                             "FROM manager_apply_fence_released_records "
                             "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto release_intent_digest = row.Text(0);
    const auto released_encoding = row.Text(1);
    const auto stored_digest = row.Text(2);
    if (row.Step() || apply_generation <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored fence-released record is structurally invalid");
    }
    try {
      ManagerApplyFenceReleasedDTO released{
          ContentDigest(release_intent_digest),
          DecodeFenceReleased(released_encoding),
      };
      const auto encoded = EncodeFenceReleasedRecord(released);
      const auto intent = FenceReleaseIntentLocked(apply_id, apply_generation);
      const auto &owner = released.released_fence.request
                              .reconciliation_evidence.owner_transaction;
      if (encoded.digest != stored_digest || owner.apply_id != apply_id ||
          owner.apply_generation.value() !=
              static_cast<std::uint64_t>(apply_generation) ||
          !intent.has_value() ||
          intent->fence_release_intent_digest !=
              released.fence_release_intent_digest ||
          intent->fence_release_intent.release_request !=
              released.released_fence.request) {
        throw ManagerApplyJournalIntegrityError(
            "stored fence release receipt is not bound to its exact intent");
      }
      return PersistedManagerApplyFenceReleased{std::move(released),
                                                ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored fence release receipt contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplyTerminalOutcome>
  TerminalOutcomeLocked(const ApplyId &apply_id,
                        const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyTerminalOutcome>(
        {apply_id, apply_generation, 0}, [&] {
          return TerminalOutcomeUncachedLocked(apply_id, apply_generation);
        });
  }

  std::optional<PersistedManagerApplyTerminalOutcome>
  TerminalOutcomeUncachedLocked(const ApplyId &apply_id,
                                const std::int64_t apply_generation) const {
    Statement row(database_,
                  "SELECT fence_released_digest,scene_completion_digest,"
                  "publication_receipt_digest,candidate_retained_bundle_id,"
                  "candidate_graph_revision,candidate_scene_id,"
                  "candidate_scene_revision_id,candidate_scene_content_hash,"
                  "intent_digest,failure_phase,failure_code,failure_message,"
                  "effect_disposition,outcome_state,terminal_outcome_digest,"
                  "cause_code,cause_details,producer,effect_boundary "
                  "FROM manager_apply_terminal_outcomes "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto state = row.Text(13);
    const auto stored_digest = row.Text(14);
    if (apply_generation <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored terminal Apply outcome is structurally invalid");
    }
    try {
      ManagerApplyTerminalOutcomeDTO outcome{
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          ParseTerminalOutcomeState(state),
          std::nullopt,
      };
      if (outcome.state == ManagerApplyTerminalOutcomeState::kSucceeded) {
        const auto candidate_scene_revision = row.Integer(6);
        if (candidate_scene_revision <= 0) {
          throw ManagerApplyJournalIntegrityError(
              "stored terminal Apply outcome has an invalid scene revision");
        }
        outcome.candidate_projection_revision = ProjectionRevisionDTO{
            RetainedBundleId(row.Text(3)),
            GraphRevision(row.Text(4)),
            SceneRevision{
                SceneId(row.Text(5)),
                static_cast<std::uint64_t>(candidate_scene_revision),
                ContentDigest(row.Text(7)),
            },
        };
        outcome.publication_receipt_digest = ContentDigest(row.Text(2));
        outcome.scene_completion_digest = ContentDigest(row.Text(1));
        outcome.fence_released_digest = ContentDigest(row.Text(0));
      } else {
        const auto disposition = ParseEffectDisposition(row.Text(12));
        const auto required_disposition =
            outcome.state == ManagerApplyTerminalOutcomeState::kFailed
                ? ManagerApplyEffectDisposition::kKnown
                : ManagerApplyEffectDisposition::kUnknown;
        if (disposition != required_disposition) {
          throw ManagerApplyJournalIntegrityError(
              "stored terminal Apply failure disposition does not match its "
              "state");
        }
        outcome.failure = ManagerApplyFailureEvidenceDTO{
            ContentDigest(row.Text(8)),
            ParseManagerApplyPhase(row.Text(9)),
            row.Text(10),
            row.Text(11),
            disposition,
            row.Text(15),
            DecodeCauseDetails(row.Text(16)),
            row.Text(17),
            ParseEffectBoundary(row.Text(18)),
        };
      }
      const auto encoded = EncodeTerminalOutcome(outcome);
      if (encoded.digest != stored_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored terminal Apply outcome digest is inconsistent");
      }
      if (outcome.state == ManagerApplyTerminalOutcomeState::kSucceeded) {
        const auto released = FenceReleasedLocked(apply_id, apply_generation);
        const auto publication =
            PublicationReceiptLocked(apply_id, apply_generation);
        if (!released.has_value() ||
            released->fence_released_digest != outcome.fence_released_digest ||
            !publication.has_value() ||
            publication->publication_receipt_digest !=
                outcome.scene_completion_digest ||
            publication->publication.publication_receipt.request
                    .candidate_projection_revision !=
                outcome.candidate_projection_revision ||
            publication->publication_receipt_digest !=
                outcome.publication_receipt_digest) {
          throw ManagerApplyJournalIntegrityError(
              "stored terminal Apply outcome is not bound to exact completion "
              "evidence");
        }
      } else {
        const auto intent = IntentLocked(apply_id, apply_generation);
        if (!intent.has_value() ||
            intent->intent_digest != outcome.failure->intent_digest) {
          throw ManagerApplyJournalIntegrityError(
              "stored terminal Apply failure is not bound to its exact "
              "intent");
        }
      }
      if (row.Step()) {
        throw ManagerApplyJournalIntegrityError(
            "stored terminal Apply outcome is not unique");
      }
      return PersistedManagerApplyTerminalOutcome{std::move(outcome),
                                                  ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored terminal Apply outcome contains an invalid value");
    }
  }

  std::optional<PersistedFailedApplyRelease>
  FailedApplyReleaseLocked(const ApplyId &apply_id,
                           const std::int64_t apply_generation) const {
    Statement row(database_,
                  "SELECT terminal_outcome_digest,fence_acceptance_digest,"
                  "acknowledgement_request_id,acknowledgement_source_label,"
                  "release_request_encoding,released_fence_encoding,acknowledged "
                  "FROM manager_apply_failed_release "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) return std::nullopt;
    const auto terminal_digest = row.Text(0);
    const auto acceptance_digest = row.Text(1);
    const auto acknowledgement_request_id = row.Text(2);
    const auto acknowledgement_source_label = row.Text(3);
    ValidateBoundedIdentity(acknowledgement_request_id, "acknowledgement_request_id");
    ValidateBoundedText(acknowledgement_source_label, kMaximumManagerApplySourceLabelBytes,
                        "acknowledgement_source_label");
    const auto request = DecodeFenceReleaseRequest(row.Text(4));
    const auto released_encoding = row.OptionalText(5);
    const auto acknowledged = row.Integer(6);
    if (row.Step() || apply_generation <= 0 || acknowledged < 0 || acknowledged > 1 ||
        (acknowledged == 1 && !released_encoding))
      throw ManagerApplyJournalIntegrityError("failed Apply release row is invalid");
    const auto status = StatusLocked(apply_id, apply_generation);
    if (!status || !HasFailedApplyReleaseEvidence(*status) ||
        status->terminal_outcome->terminal_outcome_digest.value() != terminal_digest ||
        status->fence_acceptance->acceptance_digest.value() != acceptance_digest)
      throw ManagerApplyJournalIntegrityError("failed Apply release has no before-effect owner evidence");
    const auto &evidence = request.reconciliation_evidence;
    const auto &accepted = status->fence_acceptance->accepted_fence;
    const auto &predecessor = status->intent.intent.expected_active_projection_revision;
    if (evidence.owner_transaction.apply_id != apply_id ||
        evidence.owner_transaction.apply_generation.value() != static_cast<std::uint64_t>(apply_generation) ||
        evidence.configuration_fence_id != accepted.configuration_fence_id ||
        evidence.configuration_fence_generation != accepted.configuration_fence_generation ||
        evidence.reconciled_scope != accepted.captured_scope ||
        evidence.active_projection_revision.retained_bundle_id != predecessor.retained_bundle_id ||
        evidence.active_projection_revision.graph_revision != predecessor.graph_revision ||
        evidence.active_projection_revision.scene_revision.scene_id != predecessor.scene_revision.scene_id)
      throw ManagerApplyJournalIntegrityError("failed Apply release does not name its held predecessor");
    std::optional<ConfigurationFenceReleasedDTO> released;
    if (released_encoding) {
      released = DecodeFenceReleased(*released_encoding);
      if (released->request != request)
        throw ManagerApplyJournalIntegrityError("failed Apply release receipt differs from its request");
    }
    return PersistedFailedApplyRelease{ContentDigest(terminal_digest),
                                       ContentDigest(acceptance_digest),
                                       RequestId(acknowledgement_request_id),
                                       acknowledgement_source_label, request,
                                       std::move(released), acknowledged == 1};
  }

  std::optional<PersistedManagerApplyStatus>
  StatusWithFailedReleaseLocked(const ApplyId &apply_id,
                                const std::int64_t generation) const {
    const auto status = StatusLocked(apply_id, generation);
    if (status)
      static_cast<void>(FailedApplyReleaseLocked(apply_id, generation));
    return status;
  }

  // Writes retain the existing page-image and cardinality limits without
  // decoding unrelated historical records. Full verification runs at open
  // and on explicit VerifyIntegrity, including startup after a crash.
  void VerifyWriteLimitsLocked() const {
    const auto page_count = ScalarInteger(database_, "PRAGMA page_count");
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    if (page_count < 0 || page_size <= 0 ||
        page_count >
            kMaximumManagerApplyJournalDatabasePageImageBytes / page_size) {
      throw ManagerApplyJournalLimitError(
          "SQLite main database page image exceeds its ceiling");
    }
    const auto intent_count =
        ScalarInteger(database_, "SELECT COUNT(*) FROM manager_apply_intents");
    if (intent_count < 0 ||
        intent_count > static_cast<std::int64_t>(kMaximumManagerApplyIntents)) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-journal cardinality is invalid");
    }

    const auto check_count = [&](const char *sql, const std::size_t maximum) {
      const auto count = ScalarInteger(database_, sql);
      if (count < 0 || count > static_cast<std::int64_t>(maximum))
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-fence journal cardinality is invalid");
    };
    check_count("SELECT COUNT(*) FROM manager_apply_affected_scopes",
                kMaximumManagerApplyAffectedScopes);
    check_count("SELECT COUNT(*) FROM manager_apply_fence_attempts",
                kMaximumManagerApplyFenceAttempts);
    check_count("SELECT COUNT(*) FROM manager_apply_fence_acceptances",
                kMaximumManagerApplyFenceAttempts);
    check_count("SELECT COUNT(*) FROM manager_apply_runtime_convergence_intents",
                kMaximumManagerApplyIntents);
    check_count("SELECT COUNT(*) FROM manager_apply_runtime_staged_ready_sets",
                kMaximumRuntimeStagedReadySets);
    check_count("SELECT COUNT(*) FROM manager_apply_private_materializations",
                kMaximumManagerApplyPrivateMaterializations);
    check_count("SELECT COUNT(*) FROM manager_apply_publication_receipts",
                kMaximumManagerApplyPublicationReceipts);
    check_count("SELECT COUNT(*) FROM manager_apply_fence_release_intents",
                kMaximumManagerApplyFenceReleaseIntents);
    check_count("SELECT COUNT(*) FROM manager_apply_fence_released_records",
                kMaximumManagerApplyFenceReleasedRecords);
    check_count("SELECT COUNT(*) FROM manager_apply_terminal_outcomes",
                kMaximumManagerApplyTerminalOutcomes);
    check_count("SELECT COUNT(*) FROM manager_apply_failed_release",
                kMaximumManagerApplyTerminalOutcomes);
  }

  ManagerApplyJournalIntegrityReport VerifyIntegrityLocked() const {
    JournalTimingSpan timing("manager_apply_integrity_total_intent_count");
    Statement integrity(database_, "PRAGMA integrity_check");
    if (!integrity.Step() || integrity.Text(0) != "ok" || integrity.Step()) {
      throw ManagerApplyJournalIntegrityError(
          "Apply-journal SQLite integrity_check failed");
    }
    Statement foreign_keys(database_, "PRAGMA foreign_key_check");
    if (foreign_keys.Step()) {
      throw ManagerApplyJournalIntegrityError(
          "Apply-journal SQLite foreign-key check failed");
    }
    ValidateMetadataLocked();
    if (ReadSchemaObjects(database_) != ExpectedSchemaObjects()) {
      throw ManagerApplyJournalIntegrityError(
          "SQLite schema objects differ from the versioned Apply artifact");
    }
    const auto page_count = ScalarInteger(database_, "PRAGMA page_count");
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    if (page_count < 0 || page_size <= 0 ||
        page_count >
            kMaximumManagerApplyJournalDatabasePageImageBytes / page_size) {
      throw ManagerApplyJournalLimitError(
          "SQLite main database page image exceeds its ceiling");
    }
    const auto intent_count =
        ScalarInteger(database_, "SELECT COUNT(*) FROM manager_apply_intents");
    timing.Rows(intent_count);
    const auto distinct_apply_ids = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM ("
        "SELECT apply_id FROM manager_apply_intents GROUP BY apply_id)");
    if (intent_count < 0 ||
        intent_count > static_cast<std::int64_t>(kMaximumManagerApplyIntents) ||
        distinct_apply_ids < 0 || distinct_apply_ids > intent_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-journal cardinality is invalid");
    }

    Statement rows(
        database_,
        "SELECT apply_id,apply_generation,request_owner_epoch,request_id "
        "FROM manager_apply_intents "
        "ORDER BY apply_id,apply_generation LIMIT ?");
    rows.Bind(1, static_cast<std::int64_t>(kMaximumManagerApplyIntents + 1U));
    std::map<ApplyId, std::int64_t> expected_generations;
    std::set<std::pair<RequestOwnerEpoch, RequestId>> request_identities;
    std::int64_t observed = 0;
    while (rows.Step()) {
      ++observed;
      if (observed > static_cast<std::int64_t>(kMaximumManagerApplyIntents)) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply intent inventory exceeds its ceiling");
      }
      const auto apply_id = ApplyId(rows.Text(0));
      const auto generation = rows.Integer(1);
      const auto request_owner_epoch = RequestOwnerEpoch(rows.Text(2));
      const auto request_id = RequestId(rows.Text(3));
      auto [expected, inserted] = expected_generations.emplace(apply_id, 1);
      if (generation != expected->second) {
        throw ManagerApplyJournalIntegrityError(
            "Apply generations are not contiguous for one Apply identity");
      }
      ++expected->second;
      if (!request_identities.emplace(request_owner_epoch, request_id).second) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply request identity is duplicated");
      }
      if (!IntentLocked(apply_id, generation).has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "Apply intent inventory references a missing record");
      }
      static_cast<void>(inserted);
    }
    if (observed != intent_count ||
        expected_generations.size() !=
            static_cast<std::size_t>(distinct_apply_ids) ||
        request_identities.size() != static_cast<std::size_t>(intent_count)) {
      throw ManagerApplyJournalIntegrityError(
          "Apply intent inventory is inconsistent");
    }
    const auto affected_scope_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_affected_scopes");
    const auto attempt_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_fence_attempts");
    const auto acceptance_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_fence_acceptances");
    const auto runtime_intent_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_apply_runtime_convergence_intents");
    const auto runtime_staged_ready_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_apply_runtime_staged_ready_sets");
    const auto private_materialization_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_apply_private_materializations");
    const auto publication_receipt_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_publication_receipts");
    const auto fence_release_intent_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_fence_release_intents");
    const auto fence_released_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_fence_released_records");
    const auto terminal_outcome_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_terminal_outcomes");
    if (affected_scope_count < 0 || attempt_count < 0 || acceptance_count < 0 ||
        runtime_intent_count < 0 || runtime_staged_ready_count < 0 ||
        private_materialization_count < 0 || publication_receipt_count < 0 ||
        fence_release_intent_count < 0 || fence_released_count < 0 ||
        terminal_outcome_count < 0 ||
        affected_scope_count >
            static_cast<std::int64_t>(kMaximumManagerApplyAffectedScopes) ||
        attempt_count >
            static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts) ||
        acceptance_count > attempt_count ||
        runtime_intent_count > acceptance_count ||
        runtime_intent_count >
            static_cast<std::int64_t>(kMaximumManagerApplyIntents) ||
        runtime_staged_ready_count >
            static_cast<std::int64_t>(kMaximumRuntimeStagedReadySets) ||
        private_materialization_count >
            static_cast<std::int64_t>(
                kMaximumManagerApplyPrivateMaterializations) ||
        publication_receipt_count >
            static_cast<std::int64_t>(
                kMaximumManagerApplyPublicationReceipts) ||
        fence_release_intent_count >
            static_cast<std::int64_t>(
                kMaximumManagerApplyFenceReleaseIntents) ||
        fence_released_count > static_cast<std::int64_t>(
                                   kMaximumManagerApplyFenceReleasedRecords) ||
        terminal_outcome_count >
            static_cast<std::int64_t>(kMaximumManagerApplyTerminalOutcomes) ||
        terminal_outcome_count > intent_count ||
        fence_released_count > fence_release_intent_count ||
        fence_release_intent_count > publication_receipt_count ||
        publication_receipt_count > private_materialization_count ||
        private_materialization_count > runtime_staged_ready_count ||
        attempt_count > affected_scope_count ||
        affected_scope_count > intent_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-fence journal cardinality is invalid");
    }
    Statement affected_scopes(
        database_,
        "SELECT apply_id,apply_generation FROM manager_apply_affected_scopes "
        "ORDER BY apply_id,apply_generation LIMIT ?");
    affected_scopes.Bind(
        1, static_cast<std::int64_t>(kMaximumManagerApplyAffectedScopes + 1U));
    std::int64_t observed_affected_scopes = 0;
    while (affected_scopes.Step()) {
      ++observed_affected_scopes;
      if (observed_affected_scopes >
              static_cast<std::int64_t>(kMaximumManagerApplyAffectedScopes) ||
          !AffectedScopeLocked(ApplyId(affected_scopes.Text(0)),
                               affected_scopes.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply affected-scope inventory is inconsistent");
      }
    }
    Statement attempts(
        database_,
        "SELECT apply_id,apply_generation FROM manager_apply_fence_attempts "
        "ORDER BY apply_id,apply_generation LIMIT ?");
    attempts.Bind(
        1, static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts + 1U));
    std::int64_t observed_attempts = 0;
    while (attempts.Step()) {
      ++observed_attempts;
      if (observed_attempts >
              static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts) ||
          !FenceAttemptLocked(ApplyId(attempts.Text(0)), attempts.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-fence attempt inventory is inconsistent");
      }
    }
    Statement acceptances(database_,
                          "SELECT apply_id,apply_generation "
                          "FROM manager_apply_fence_acceptances "
                          "ORDER BY apply_id,apply_generation LIMIT ?");
    acceptances.Bind(
        1, static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts + 1U));
    std::int64_t observed_acceptances = 0;
    while (acceptances.Step()) {
      ++observed_acceptances;
      if (observed_acceptances >
              static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts) ||
          !FenceAcceptanceLocked(ApplyId(acceptances.Text(0)),
                                 acceptances.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-fence acceptance inventory is inconsistent");
      }
    }
    if (observed_affected_scopes != affected_scope_count ||
        observed_attempts != attempt_count ||
        observed_acceptances != acceptance_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-fence inventory count is inconsistent");
    }
    Statement runtime_intents(database_,
                              "SELECT apply_id,apply_generation "
                              "FROM manager_apply_runtime_convergence_intents "
                              "ORDER BY apply_id,apply_generation LIMIT ?");
    runtime_intents.Bind(
        1, static_cast<std::int64_t>(kMaximumManagerApplyIntents + 1U));
    std::int64_t observed_runtime_intents = 0;
    while (runtime_intents.Step()) {
      ++observed_runtime_intents;
      if (observed_runtime_intents >
              static_cast<std::int64_t>(kMaximumManagerApplyIntents) ||
          !RuntimeConvergenceIntentLocked(ApplyId(runtime_intents.Text(0)),
                                          runtime_intents.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored runtime convergence-intent inventory is inconsistent");
      }
    }
    Statement staged_ready_sets(
        database_, "SELECT apply_id,apply_generation,staged_ready_generation "
                   "FROM manager_apply_runtime_staged_ready_sets "
                   "ORDER BY apply_id,apply_generation,staged_ready_generation "
                   "LIMIT ?");
    staged_ready_sets.Bind(
        1, static_cast<std::int64_t>(kMaximumRuntimeStagedReadySets + 1U));
    std::int64_t observed_staged_ready = 0;
    std::map<std::pair<ApplyId, std::int64_t>, std::int64_t>
        expected_staged_ready_generations;
    while (staged_ready_sets.Step()) {
      ++observed_staged_ready;
      const auto apply = ApplyId(staged_ready_sets.Text(0));
      const auto apply_generation = staged_ready_sets.Integer(1);
      const auto staged_generation = staged_ready_sets.Integer(2);
      auto [expected, inserted] = expected_staged_ready_generations.emplace(
          std::make_pair(apply, apply_generation), 1);
      if (observed_staged_ready >
              static_cast<std::int64_t>(kMaximumRuntimeStagedReadySets) ||
          staged_generation != expected->second ||
          staged_generation > static_cast<std::int64_t>(
                                  kMaximumRuntimeStagedReadySetsPerApply) ||
          !RuntimeStagedReadyLocked(apply, apply_generation, staged_generation)
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored runtime staged-ready inventory is inconsistent");
      }
      ++expected->second;
      static_cast<void>(inserted);
    }
    if (observed_runtime_intents != runtime_intent_count ||
        observed_staged_ready != runtime_staged_ready_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored runtime convergence inventory count is inconsistent");
    }
    Statement private_materializations(
        database_, "SELECT apply_id,apply_generation "
                   "FROM manager_apply_private_materializations "
                   "ORDER BY apply_id,apply_generation LIMIT ?");
    private_materializations.Bind(
        1, static_cast<std::int64_t>(
               kMaximumManagerApplyPrivateMaterializations + 1U));
    std::int64_t observed_private_materializations = 0;
    while (private_materializations.Step()) {
      ++observed_private_materializations;
      if (observed_private_materializations >
              static_cast<std::int64_t>(
                  kMaximumManagerApplyPrivateMaterializations) ||
          !PrivateMaterializationLocked(
               ApplyId(private_materializations.Text(0)),
               private_materializations.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored private-materialization inventory is inconsistent");
      }
    }
    Statement publication_receipts(
        database_, "SELECT apply_id,apply_generation "
                   "FROM manager_apply_publication_receipts "
                   "ORDER BY apply_id,apply_generation LIMIT ?");
    publication_receipts.Bind(
        1, static_cast<std::int64_t>(kMaximumManagerApplyPublicationReceipts +
                                     1U));
    std::int64_t observed_publication_receipts = 0;
    while (publication_receipts.Step()) {
      ++observed_publication_receipts;
      if (observed_publication_receipts >
              static_cast<std::int64_t>(
                  kMaximumManagerApplyPublicationReceipts) ||
          !PublicationReceiptLocked(ApplyId(publication_receipts.Text(0)),
                                    publication_receipts.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-publication inventory is inconsistent");
      }
    }
    if (observed_private_materializations != private_materialization_count ||
        observed_publication_receipts != publication_receipt_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored selected-bundle transition inventory is inconsistent");
    }
    Statement fence_release_intents(
        database_, "SELECT apply_id,apply_generation "
                   "FROM manager_apply_fence_release_intents "
                   "ORDER BY apply_id,apply_generation LIMIT ?");
    fence_release_intents.Bind(
        1, static_cast<std::int64_t>(kMaximumManagerApplyFenceReleaseIntents +
                                     1U));
    std::int64_t observed_fence_release_intents = 0;
    while (fence_release_intents.Step()) {
      ++observed_fence_release_intents;
      if (observed_fence_release_intents >
              static_cast<std::int64_t>(
                  kMaximumManagerApplyFenceReleaseIntents) ||
          !FenceReleaseIntentLocked(ApplyId(fence_release_intents.Text(0)),
                                    fence_release_intents.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored fence-release intent inventory is inconsistent");
      }
    }
    Statement fence_released_records(
        database_, "SELECT apply_id,apply_generation "
                   "FROM manager_apply_fence_released_records "
                   "ORDER BY apply_id,apply_generation LIMIT ?");
    fence_released_records.Bind(
        1, static_cast<std::int64_t>(kMaximumManagerApplyFenceReleasedRecords +
                                     1U));
    std::int64_t observed_fence_released = 0;
    while (fence_released_records.Step()) {
      ++observed_fence_released;
      if (observed_fence_released >
              static_cast<std::int64_t>(
                  kMaximumManagerApplyFenceReleasedRecords) ||
          !FenceReleasedLocked(ApplyId(fence_released_records.Text(0)),
                               fence_released_records.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored fence-released inventory is inconsistent");
      }
    }
    Statement terminal_outcomes(database_,
                                "SELECT apply_id,apply_generation "
                                "FROM manager_apply_terminal_outcomes "
                                "ORDER BY apply_id,apply_generation LIMIT ?");
    terminal_outcomes.Bind(1, static_cast<std::int64_t>(
                                  kMaximumManagerApplyTerminalOutcomes + 1U));
    std::int64_t observed_terminal_outcomes = 0;
    while (terminal_outcomes.Step()) {
      ++observed_terminal_outcomes;
      if (observed_terminal_outcomes >
              static_cast<std::int64_t>(kMaximumManagerApplyTerminalOutcomes) ||
          !TerminalOutcomeLocked(ApplyId(terminal_outcomes.Text(0)),
                                 terminal_outcomes.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored terminal Apply outcome inventory is inconsistent");
      }
    }
    if (observed_fence_release_intents != fence_release_intent_count ||
        observed_fence_released != fence_released_count ||
        observed_terminal_outcomes != terminal_outcome_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply completion inventory is inconsistent");
    }
    Statement failed_releases(database_,
                              "SELECT apply_id,apply_generation FROM manager_apply_failed_release "
                              "ORDER BY apply_id,apply_generation LIMIT ?");
    failed_releases.Bind(1, static_cast<std::int64_t>(kMaximumManagerApplyTerminalOutcomes + 1U));
    std::size_t failed_release_count = 0U;
    while (failed_releases.Step()) {
      if (++failed_release_count > kMaximumManagerApplyTerminalOutcomes ||
          !FailedApplyReleaseLocked(ApplyId(failed_releases.Text(0)), failed_releases.Integer(1)))
        throw ManagerApplyJournalIntegrityError("stored failed Apply release inventory is inconsistent");
    }
    // These are table cardinalities already read by the integrity check. The
    // elapsed time is for the whole check, not an individual COUNT query.
    timing.Count("manager_apply_integrity_scope_count", affected_scope_count);
    timing.Count("manager_apply_integrity_attempt_count", attempt_count);
    timing.Count("manager_apply_integrity_acceptance_count", acceptance_count);
    timing.Count("manager_apply_integrity_runtime_intent_count", runtime_intent_count);
    timing.Count("manager_apply_integrity_staged_count", runtime_staged_ready_count);
    timing.Count("manager_apply_integrity_materialization_count", private_materialization_count);
    timing.Count("manager_apply_integrity_publication_count", publication_receipt_count);
    timing.Count("manager_apply_integrity_release_intent_count", fence_release_intent_count);
    timing.Count("manager_apply_integrity_released_count", fence_released_count);
    timing.Count("manager_apply_integrity_terminal_count", terminal_outcome_count);
    timing.Count("manager_apply_integrity_failed_release_count",
                 static_cast<std::int64_t>(failed_release_count));
    return {
        cell_id_,
        std::string(kManagerApplyJournalSchemaId),
        kManagerApplyJournalSchemaVersion,
        RawDigest(detail::kEmbeddedManagerApplyJournalSchema),
        intent_count,
        distinct_apply_ids,
        static_cast<std::int64_t>(request_identities.size()),
        affected_scope_count,
        attempt_count,
        acceptance_count,
        runtime_intent_count,
        runtime_staged_ready_count,
        private_materialization_count,
        publication_receipt_count,
        fence_release_intent_count,
        fence_released_count,
        terminal_outcome_count,
    };
  }

  std::filesystem::path database_path_;
  CellId cell_id_;
  sqlite3 *database_{};
  mutable std::mutex mutex_;
  mutable VerifiedReads *verified_reads_{};
  bool verify_write_generation_predecessors_{};
  bool closed_{};
  bool borrowed_{};

  /// Verified records of the last read transactions and the database change
  /// identity they were read under. Guarded by `mutex_`.
  struct ChangeGatedReads final {
    std::int64_t data_version{};
    sqlite3_int64 own_total_changes{};
    VerifiedReads reads;
  };
  mutable std::optional<ChangeGatedReads> change_gated_reads_;
  mutable std::uint64_t record_verifications_{};
};

SqliteManagerApplyJournal::SqliteManagerApplyJournal(
    std::filesystem::path database_path, CellId cell_id)
    : impl_(std::make_unique<Impl>(std::move(database_path),
                                   std::move(cell_id))) {}

SqliteManagerApplyJournal::~SqliteManagerApplyJournal() = default;

SqliteManagerApplyJournal::SqliteManagerApplyJournal(
    SqliteManagerApplyJournal &&) noexcept = default;

SqliteManagerApplyJournal &SqliteManagerApplyJournal::operator=(
    SqliteManagerApplyJournal &&) noexcept = default;

const std::filesystem::path &
SqliteManagerApplyJournal::database_path() const noexcept {
  return impl_->database_path_;
}

const CellId &SqliteManagerApplyJournal::cell_id() const noexcept {
  return impl_->cell_id_;
}

ManagerApplyIntentWriteResult
SqliteManagerApplyJournal::EnsureIntent(const ManagerApplyIntentDTO &intent) {
  // Preserve the ordinary acceptance/replay semantics and digest bytes.
  static_cast<void>(EncodeIntent(intent));
  if (intent.cell_id != impl_->cell_id_) {
    throw ManagerApplyJournalConflictError(
        "Apply intent belongs to a different cell");
  }
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto result = impl_->EnsureIntentValueLocked(intent);
  if (result.inserted)
    static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
  return result;
}

std::optional<PersistedManagerApplyIntent>
SqliteManagerApplyJournal::ReadIntent(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->IntentLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

std::vector<PersistedManagerApplyStatus>
SqliteManagerApplyJournal::ReadStatusInventory() const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();

  Statement rows(impl_->database_,
                 "SELECT apply_id,apply_generation FROM manager_apply_intents "
                 "ORDER BY apply_id,apply_generation LIMIT ?");
  rows.Bind(1, static_cast<std::int64_t>(kMaximumManagerApplyIntents + 1U));
  std::vector<std::pair<ApplyId, std::int64_t>> keys;
  keys.reserve(kMaximumManagerApplyIntents);
  while (rows.Step()) {
    if (keys.size() == kMaximumManagerApplyIntents) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply status inventory exceeds its ceiling");
    }
    keys.emplace_back(ApplyId(rows.Text(0)), rows.Integer(1));
  }

  std::vector<PersistedManagerApplyStatus> result;
  result.reserve(keys.size());
  for (const auto &[apply_id, generation] : keys) {
    auto status = impl_->StatusLocked(apply_id, generation);
    if (!status.has_value()) {
      throw ManagerApplyJournalIntegrityError(
          "Apply status inventory references a missing intent");
    }
    result.push_back(std::move(*status));
  }
  transaction.Commit();
  return result;
}

std::uint64_t SqliteManagerApplyJournal::record_verification_count() const {
  std::lock_guard lock(impl_->mutex_);
  return impl_->record_verifications_;
}

std::optional<PersistedManagerApplyStatus>
SqliteManagerApplyJournal::ReadStatus(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  auto status = impl_->StatusLocked(apply_id, generation);
  transaction.Commit();
  return status;
}

std::optional<PersistedManagerApplyIntent>
SqliteManagerApplyJournal::ReadRequest(
    const RequestOwnerEpoch &request_owner_epoch,
    const RequestId &request_id) const {
  ValidateBoundedIdentity(request_owner_epoch.value(), "request_owner_epoch");
  ValidateBoundedIdentity(request_id.value(), "request_id");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->RequestLocked(request_owner_epoch, request_id);
  transaction.Commit();
  return result;
}

std::optional<ApplyGeneration>
SqliteManagerApplyJournal::ReadApplyGenerationHighWater(
    const ApplyId &apply_id) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->GenerationHighWaterLocked(apply_id);
  transaction.Commit();
  if (!result.has_value()) {
    return std::nullopt;
  }
  return ApplyGeneration(static_cast<std::uint64_t>(*result));
}

ManagerApplyAffectedScopeWriteResult
SqliteManagerApplyJournal::EnsureAffectedScope(
    const ContentDigest &intent_digest,
    const ManagerApplyAffectedScopeDTO &affected_scope) {
  const auto encoded = EncodeAffectedScope(intent_digest, affected_scope);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto parent =
      impl_->IntentLocked(affected_scope.apply_id, encoded.apply_generation);
  if (!parent.has_value() || parent->intent_digest != intent_digest) {
    throw ManagerApplyJournalConflictError(
        "Apply affected scope does not name its exact durable intent");
  }
  if (const auto existing = impl_->AffectedScopeLocked(
          affected_scope.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->intent_digest != intent_digest ||
        existing->affected_scope_digest.value() != encoded.digest ||
        existing->affected_scope != affected_scope) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has a different affected scope");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto count = ScalarInteger(
      impl_->database_, "SELECT COUNT(*) FROM manager_apply_affected_scopes");
  if (count >= static_cast<std::int64_t>(kMaximumManagerApplyAffectedScopes)) {
    throw ManagerApplyJournalLimitError(
        "Apply affected-scope inventory reached its fixed limit");
  }
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_affected_scopes ("
      "apply_id,apply_generation,intent_digest,components_encoding,"
      "domains_encoding,bindings_encoding,resources_encoding,"
      "busy_disposition,affected_scope_digest,component_delta_encoding) "
      "VALUES (?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, affected_scope.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, intent_digest.value());
  insert.Bind(4, encoded.components);
  insert.Bind(5, encoded.domains);
  insert.Bind(6, encoded.bindings);
  insert.Bind(7, encoded.resources);
  insert.Bind(8, encoded.busy_disposition);
  insert.Bind(9, encoded.digest);
  insert.Bind(10, encoded.component_delta);
  static_cast<void>(insert.Step());
  const auto stored = impl_->AffectedScopeLocked(affected_scope.apply_id,
                                                 encoded.apply_generation);
  if (!stored.has_value() ||
      stored->affected_scope_digest.value() != encoded.digest ||
      stored->affected_scope != affected_scope) {
    throw ManagerApplyJournalIntegrityError(
        "inserted Apply affected scope cannot be verified");
  }
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyAffectedScope>
SqliteManagerApplyJournal::ReadAffectedScope(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->AffectedScopeLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyFenceAttemptWriteResult
SqliteManagerApplyJournal::EnsureFenceAttempt(
    const ContentDigest &intent_digest,
    const ContentDigest &affected_scope_digest,
    const ApplyConfigurationFenceIntentDTO &fence_intent) {
  const auto encoded =
      EncodeFenceAttempt(intent_digest, affected_scope_digest, fence_intent);
  const auto &apply_id = fence_intent.owner_transaction.apply_id;
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto parent = impl_->IntentLocked(apply_id, encoded.apply_generation);
  if (!parent.has_value() || parent->intent_digest != intent_digest) {
    throw ManagerApplyJournalConflictError(
        "Apply-fence attempt does not name its exact durable intent");
  }
  const auto affected_scope =
      impl_->AffectedScopeLocked(apply_id, encoded.apply_generation);
  if (!affected_scope.has_value() ||
      affected_scope->affected_scope_digest != affected_scope_digest ||
      affected_scope->affected_scope.scope != fence_intent.scope ||
      affected_scope->affected_scope.busy_disposition !=
          fence_intent.busy_disposition) {
    throw ManagerApplyJournalConflictError(
        "Apply-fence attempt conflicts with its durable affected scope");
  }
  if (parent->intent.request_context != fence_intent.owner_request_context ||
      parent->intent.expected_active_projection_revision.retained_bundle_id !=
          fence_intent.expected_retained_bundle_id ||
      parent->intent.expected_active_projection_revision.graph_revision !=
          fence_intent.expected_graph_revision) {
    throw ManagerApplyJournalConflictError(
        "Apply-fence attempt conflicts with its durable Apply intent");
  }
  if (const auto existing =
          impl_->FenceAttemptLocked(apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->intent_digest != intent_digest ||
        existing->affected_scope_digest != affected_scope_digest ||
        existing->fence_attempt_digest.value() != encoded.digest ||
        !SameFenceIntent(existing->fence_intent, fence_intent)) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has a different durable fence attempt");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto attempt_count = ScalarInteger(
      impl_->database_, "SELECT COUNT(*) FROM manager_apply_fence_attempts");
  if (attempt_count >=
      static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts)) {
    throw ManagerApplyJournalLimitError(
        "Apply-fence attempt inventory reached its fixed limit");
  }
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_fence_attempts ("
      "apply_id,apply_generation,intent_digest,affected_scope_digest,request_"
      "id,"
      "request_owner_epoch,source_label,correlation_id,"
      "components_encoding,domains_encoding,bindings_encoding,"
      "resources_encoding,busy_disposition,expected_retained_bundle_id,"
      "expected_graph_revision,expected_operation_resource_generation,"
      "expected_fence_generation,active_configurations_encoding,reason,"
      "fence_attempt_digest) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, intent_digest.value());
  insert.Bind(4, affected_scope_digest.value());
  insert.Bind(5, fence_intent.owner_request_context.request_id.value());
  insert.Bind(6,
              fence_intent.owner_request_context.request_owner_epoch.value());
  insert.Bind(7, fence_intent.owner_request_context.source_label);
  if (fence_intent.owner_request_context.correlation_id.has_value()) {
    insert.Bind(8, *fence_intent.owner_request_context.correlation_id);
  } else {
    insert.BindNull(8);
  }
  insert.Bind(9, encoded.components);
  insert.Bind(10, encoded.domains);
  insert.Bind(11, encoded.bindings);
  insert.Bind(12, encoded.resources);
  insert.Bind(13, core::to_string(fence_intent.busy_disposition));
  insert.Bind(14, fence_intent.expected_retained_bundle_id.value());
  insert.Bind(15, fence_intent.expected_graph_revision.value());
  insert.Bind(16, encoded.operation_generation);
  insert.Bind(17, encoded.fence_generation);
  insert.Bind(18, encoded.active_configurations);
  insert.Bind(19, fence_intent.reason);
  insert.Bind(20, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored =
      impl_->FenceAttemptLocked(apply_id, encoded.apply_generation);
  if (!stored.has_value() ||
      stored->fence_attempt_digest.value() != encoded.digest ||
      !SameFenceIntent(stored->fence_intent, fence_intent)) {
    throw ManagerApplyJournalIntegrityError(
        "inserted Apply-fence attempt cannot be verified");
  }
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyFenceAttempt>
SqliteManagerApplyJournal::ReadFenceAttempt(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->FenceAttemptLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyFenceAcceptanceWriteResult
SqliteManagerApplyJournal::EnsureFenceAcceptance(
    const ApplyId &apply_id, const ApplyGeneration apply_generation,
    const ContentDigest &fence_attempt_digest,
    const ConfigurationFenceAcceptedDTO &accepted_fence) {
  const auto encoded = EncodeFenceAcceptance(
      apply_id, apply_generation, fence_attempt_digest, accepted_fence);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto attempt =
      impl_->FenceAttemptLocked(apply_id, encoded.apply_generation);
  if (!attempt.has_value() ||
      attempt->fence_attempt_digest != fence_attempt_digest) {
    throw ManagerApplyJournalConflictError(
        "Apply-fence acceptance does not name its exact durable attempt");
  }
  if (!accepted_fence.matches(attempt->fence_intent)) {
    throw ManagerApplyJournalConflictError(
        "Apply-fence acceptance does not match its exact attempt");
  }
  if (const auto existing =
          impl_->FenceAcceptanceLocked(apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->fence_attempt_digest != fence_attempt_digest ||
        existing->acceptance_digest.value() != encoded.digest ||
        !SameAcceptedFence(existing->accepted_fence, accepted_fence)) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has different durable fence acceptance");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto acceptance_count = ScalarInteger(
      impl_->database_, "SELECT COUNT(*) FROM manager_apply_fence_acceptances");
  if (acceptance_count >=
      static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts)) {
    throw ManagerApplyJournalLimitError(
        "Apply-fence acceptance inventory reached its fixed limit");
  }
  Statement insert(impl_->database_,
                   "INSERT INTO manager_apply_fence_acceptances ("
                   "apply_id,apply_generation,fence_attempt_digest,"
                   "configuration_fence_id,configuration_fence_generation,"
                   "captured_components_encoding,captured_domains_encoding,"
                   "captured_bindings_encoding,captured_resources_encoding,"
                   "accepted_at_ns,replay_status,acceptance_digest) "
                   "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, fence_attempt_digest.value());
  insert.Bind(4, accepted_fence.configuration_fence_id.value());
  insert.Bind(5, encoded.fence_generation);
  insert.Bind(6, encoded.components);
  insert.Bind(7, encoded.domains);
  insert.Bind(8, encoded.bindings);
  insert.Bind(9, encoded.resources);
  insert.Bind(10, accepted_fence.accepted_at_ns);
  insert.Bind(11, encoded.replay_status);
  insert.Bind(12, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored =
      impl_->FenceAcceptanceLocked(apply_id, encoded.apply_generation);
  if (!stored.has_value() ||
      stored->acceptance_digest.value() != encoded.digest ||
      !SameAcceptedFence(stored->accepted_fence, accepted_fence)) {
    throw ManagerApplyJournalIntegrityError(
        "inserted Apply-fence acceptance cannot be verified");
  }
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyFenceAcceptance>
SqliteManagerApplyJournal::ReadFenceAcceptance(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->FenceAcceptanceLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyRuntimeConvergenceIntentWriteResult
SqliteManagerApplyJournal::EnsureRuntimeConvergenceIntent(
    const ContentDigest &intent_digest,
    const ManagerApplyRuntimeConvergenceIntentDTO &runtime_intent) {
  const auto encoded =
      EncodeRuntimeConvergenceIntent(intent_digest, runtime_intent);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto parent =
      impl_->IntentLocked(runtime_intent.apply_id, encoded.apply_generation);
  const auto acceptance = impl_->FenceAcceptanceLocked(
      runtime_intent.apply_id, encoded.apply_generation);
  if (!parent.has_value() || parent->intent_digest != intent_digest ||
      !acceptance.has_value() ||
      acceptance->acceptance_digest != runtime_intent.fence_acceptance_digest) {
    throw ManagerApplyJournalConflictError(
        "runtime convergence intent does not name its exact durable "
        "Apply/fence acceptance");
  }
  if (const auto existing = impl_->RuntimeConvergenceIntentLocked(
          runtime_intent.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->intent_digest != intent_digest ||
        existing->runtime_convergence_intent != runtime_intent ||
        existing->runtime_convergence_intent_digest.value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has a different runtime convergence "
          "intent");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto count = ScalarInteger(
      impl_->database_,
      "SELECT COUNT(*) FROM manager_apply_runtime_convergence_intents");
  if (count >= static_cast<std::int64_t>(kMaximumManagerApplyIntents)) {
    throw ManagerApplyJournalLimitError(
        "runtime convergence-intent inventory reached its fixed limit");
  }
  Statement insert(impl_->database_,
                   "INSERT INTO manager_apply_runtime_convergence_intents ("
                   "apply_id,apply_generation,intent_digest,"
                   "candidate_retained_bundle_id,candidate_graph_revision,"
                   "fence_acceptance_digest,predecessor_publication_generation,"
                   "predecessor_public_pointer_digest,runtime_units_encoding,"
                   "runtime_convergence_intent_digest) "
                   "VALUES (?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, runtime_intent.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, intent_digest.value());
  insert.Bind(4, runtime_intent.candidate_retained_bundle_id.value());
  insert.Bind(5, runtime_intent.candidate_graph_revision.value());
  insert.Bind(6, runtime_intent.fence_acceptance_digest.value());
  insert.Bind(7, encoded.predecessor_publication_generation);
  insert.Bind(8, runtime_intent.predecessor_public_pointer_digest.value());
  insert.Bind(9, encoded.runtime_units);
  insert.Bind(10, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->RuntimeConvergenceIntentLocked(
      runtime_intent.apply_id, encoded.apply_generation);
  if (!stored.has_value() ||
      stored->runtime_convergence_intent != runtime_intent ||
      stored->runtime_convergence_intent_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted runtime convergence intent cannot be verified");
  }
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyRuntimeConvergenceIntent>
SqliteManagerApplyJournal::ReadRuntimeConvergenceIntent(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result =
      impl_->RuntimeConvergenceIntentLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyRuntimeStagedReadyWriteResult
SqliteManagerApplyJournal::EnsureRuntimeStagedReady(
    const ManagerApplyRuntimeStagedReadyDTO &staged_ready) {
  const auto encoded = EncodeRuntimeStagedReady(staged_ready);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto intent = impl_->RuntimeConvergenceIntentLocked(
      staged_ready.apply_id, encoded.apply_generation);
  if (!intent.has_value() ||
      intent->runtime_convergence_intent_digest !=
          staged_ready.runtime_convergence_intent_digest ||
      intent->runtime_convergence_intent.predecessor_publication_generation !=
          staged_ready.predecessor_publication_generation ||
      intent->runtime_convergence_intent.predecessor_public_pointer_digest !=
          staged_ready.predecessor_public_pointer_digest) {
    throw ManagerApplyJournalConflictError(
        "runtime staged-ready set does not name its exact convergence "
        "intent/publication");
  }
  if (const auto existing = impl_->RuntimeStagedReadyLocked(
          staged_ready.apply_id, encoded.apply_generation,
          encoded.staged_ready_generation);
      existing.has_value()) {
    if (existing->staged_ready != staged_ready ||
        existing->staged_ready_digest.value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "staged-ready generation already has different evidence");
    }
    transaction.Commit();
    return {*existing, false};
  }
  // A scene-stage intent freezes the scene transition, not the Manager's
  // observation of the runtime child.  If the Manager restarts after scene
  // staging/activation, the native child can legitimately receive a new
  // process epoch; append a fresh staged-ready row while retaining the
  // original digest as the immutable scene-stage parent.
  const auto latest = impl_->LatestRuntimeStagedReadyLocked(
      staged_ready.apply_id, encoded.apply_generation, true);
  const auto expected_generation =
      latest.has_value() ? latest->staged_ready.staged_ready_generation + 1U
                         : 1U;
  if (staged_ready.staged_ready_generation != expected_generation ||
      staged_ready.staged_ready_generation >
          kMaximumRuntimeStagedReadySetsPerApply) {
    throw ManagerApplyJournalConflictError(
        "runtime staged-ready generations must be contiguous and bounded");
  }
  const auto count = ScalarInteger(
      impl_->database_,
      "SELECT COUNT(*) FROM manager_apply_runtime_staged_ready_sets");
  if (count >= static_cast<std::int64_t>(kMaximumRuntimeStagedReadySets)) {
    throw ManagerApplyJournalLimitError(
        "runtime staged-ready inventory reached its fixed limit");
  }
  Statement insert(impl_->database_,
                   "INSERT INTO manager_apply_runtime_staged_ready_sets ("
                   "apply_id,apply_generation,staged_ready_generation,"
                   "runtime_convergence_intent_digest,"
                   "predecessor_publication_generation,"
                   "predecessor_public_pointer_digest,evaluated_at_ns,"
                   "runtime_evidence_encoding,staged_ready_digest) "
                   "VALUES (?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, staged_ready.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, encoded.staged_ready_generation);
  insert.Bind(4, staged_ready.runtime_convergence_intent_digest.value());
  insert.Bind(5, encoded.predecessor_publication_generation);
  insert.Bind(6, staged_ready.predecessor_public_pointer_digest.value());
  insert.Bind(7, encoded.evaluated_at_ns);
  insert.Bind(8, encoded.runtime_evidence);
  insert.Bind(9, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->RuntimeStagedReadyLocked(
      staged_ready.apply_id, encoded.apply_generation,
      encoded.staged_ready_generation);
  if (!stored.has_value() || stored->staged_ready != staged_ready ||
      stored->staged_ready_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted runtime staged-ready set cannot be verified");
  }
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyRuntimeStagedReady>
SqliteManagerApplyJournal::ReadLatestRuntimeStagedReady(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result =
      impl_->LatestRuntimeStagedReadyLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyPrivateMaterializationWriteResult
SqliteManagerApplyJournal::EnsurePrivateMaterialization(
    const ManagerApplyPrivateMaterializationDTO &private_materialization) {
  const auto encoded = EncodePrivateMaterialization(private_materialization);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto intent = impl_->IntentLocked(
      private_materialization.apply_id, encoded.apply_generation);
  if (!intent.has_value() ||
      private_materialization.authoritative_scene_snapshot_digest !=
          private_materialization.candidate_projection_revision.scene_revision
              .content_hash ||
      intent->intent.candidate_retained_bundle_id !=
          private_materialization.candidate_projection_revision
              .retained_bundle_id ||
      intent->intent.candidate_graph_revision !=
          private_materialization.candidate_projection_revision
              .graph_revision) {
    throw ManagerApplyJournalConflictError(
        "private materialization does not match its exact Apply and "
        "authoritative Scene snapshot");
  }
  if (const auto existing = impl_->PrivateMaterializationLocked(
          private_materialization.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->private_materialization != private_materialization ||
        existing->private_materialization_content_digest.value() !=
            encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has a different private "
          "materialization");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto count = ScalarInteger(
      impl_->database_,
      "SELECT COUNT(*) FROM manager_apply_private_materializations");
  if (count >=
      static_cast<std::int64_t>(kMaximumManagerApplyPrivateMaterializations)) {
    throw ManagerApplyJournalLimitError(
        "private-materialization inventory reached its fixed limit");
  }
  const auto &candidate = private_materialization.candidate_projection_revision;
  Statement insert(impl_->database_,
                   "INSERT INTO manager_apply_private_materializations ("
                   "apply_id,apply_generation,authoritative_scene_snapshot_digest,"
                   "candidate_retained_bundle_id,"
                   "candidate_graph_revision,candidate_scene_id,"
                   "candidate_scene_revision_id,candidate_scene_content_hash,"
                   "retained_bundle_canonical_content_digest,"
                   "retained_bundle_canonical_byte_size,"
                   "private_materialization_content_digest) "
                   "VALUES (?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, private_materialization.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3,
              private_materialization.authoritative_scene_snapshot_digest.value());
  insert.Bind(4, candidate.retained_bundle_id.value());
  insert.Bind(5, candidate.graph_revision.value());
  insert.Bind(6, candidate.scene_revision.scene_id.value());
  insert.Bind(7, encoded.candidate_scene_revision_id);
  insert.Bind(8, candidate.scene_revision.content_hash.value());
  insert.Bind(
      9,
      private_materialization.retained_bundle_canonical_content_digest.value());
  insert.Bind(10, encoded.retained_bundle_canonical_byte_size);
  insert.Bind(11, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->PrivateMaterializationLocked(
      private_materialization.apply_id, encoded.apply_generation);
  if (!stored.has_value() ||
      stored->private_materialization != private_materialization ||
      stored->private_materialization_content_digest.value() !=
          encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted private materialization cannot be verified");
  }
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyPrivateMaterialization>
SqliteManagerApplyJournal::ReadPrivateMaterialization(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->PrivateMaterializationLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyPublicationReceiptWriteResult
SqliteManagerApplyJournal::EnsurePublicationReceipt(
    const ManagerApplyPublicationReceiptDTO &publication) {
  const auto encoded = EncodePublicationReceipt(publication);
  const auto &receipt = publication.publication_receipt;
  const auto &request = receipt.request;
  const auto *named = std::get_if<NamedApplyPublicationTransactionDTO>(
      &request.transaction_ref);
  if (named == nullptr) {
    throw std::invalid_argument("Manager Apply publication is not named-Apply");
  }
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto materialization = impl_->PrivateMaterializationLocked(
      named->apply_id, encoded.apply_generation);
  if (!materialization.has_value() ||
      materialization->private_materialization_content_digest !=
          publication.private_materialization_content_digest ||
      publication.private_materialization_content_digest !=
          request.private_materialization_content_digest) {
    throw ManagerApplyJournalConflictError(
        "publication receipt does not match exact private materialization");
  }
  if (const auto existing = impl_->PublicationReceiptLocked(
          named->apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->publication != publication ||
        existing->publication_receipt_digest.value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has a different publication receipt");
    }
    transaction.Commit();
    return {*existing, false};
  }
  Statement generation_owner(
      impl_->database_,
      "SELECT apply_id,apply_generation FROM "
      "manager_apply_publication_receipts WHERE publication_generation=?");
  generation_owner.Bind(1, encoded.publication_generation);
  if (generation_owner.Step()) {
    const ApplyId owner_id(generation_owner.Text(0));
    const auto owner_generation = generation_owner.Integer(1);
    if (owner_generation <= 0 || generation_owner.Step() ||
        !impl_->PublicationReceiptLocked(owner_id, owner_generation))
      throw ManagerApplyJournalIntegrityError(
          "publication generation owner is invalid");
    throw ManagerApplyJournalConflictError(
        "publication generation already belongs to another Apply");
  }
  const auto count =
      ScalarInteger(impl_->database_,
                    "SELECT COUNT(*) FROM manager_apply_publication_receipts");
  if (count >=
      static_cast<std::int64_t>(kMaximumManagerApplyPublicationReceipts)) {
    throw ManagerApplyJournalLimitError(
        "Apply-publication inventory reached its fixed limit");
  }
  const auto &candidate = request.candidate_projection_revision;
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_publication_receipts ("
      "apply_id,apply_generation,"
      "candidate_retained_bundle_id,"
      "candidate_graph_revision,candidate_scene_id,"
      "candidate_scene_revision_id,candidate_scene_content_hash,"
      "expected_previous_publication_generation,"
      "private_materialization_content_digest,publication_generation,"
      "durable_public_pointer_content_digest,publication_receipt_digest) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, named->apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, candidate.retained_bundle_id.value());
  insert.Bind(4, candidate.graph_revision.value());
  insert.Bind(5, candidate.scene_revision.scene_id.value());
  insert.Bind(6, encoded.candidate_scene_revision_id);
  insert.Bind(7, candidate.scene_revision.content_hash.value());
  insert.Bind(8, encoded.expected_previous_publication_generation);
  insert.Bind(9, request.private_materialization_content_digest.value());
  insert.Bind(10, encoded.publication_generation);
  insert.Bind(11, receipt.durable_public_pointer_content_digest.value());
  insert.Bind(12, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->PublicationReceiptLocked(named->apply_id,
                                                      encoded.apply_generation);
  if (!stored.has_value() || stored->publication != publication ||
      stored->publication_receipt_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted Apply publication receipt cannot be verified");
  }
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyPublicationReceipt>
SqliteManagerApplyJournal::ReadPublicationReceipt(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->PublicationReceiptLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyFenceReleaseIntentWriteResult
SqliteManagerApplyJournal::EnsureFenceReleaseIntent(
    const ManagerApplyFenceReleaseIntentDTO &fence_release_intent) {
  const auto encoded = EncodeFenceReleaseIntent(fence_release_intent);
  const auto &owner = fence_release_intent.release_request
                          .reconciliation_evidence.owner_transaction;
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto acceptance =
      impl_->FenceAcceptanceLocked(owner.apply_id, encoded.apply_generation);
  const auto publication =
      impl_->PublicationReceiptLocked(owner.apply_id, encoded.apply_generation);
  const auto &evidence =
      fence_release_intent.release_request.reconciliation_evidence;
  if (!publication.has_value() ||
      publication->publication_receipt_digest !=
          fence_release_intent.scene_completion_digest ||
      !acceptance.has_value() ||
      acceptance->acceptance_digest !=
          fence_release_intent.fence_acceptance_digest ||
      acceptance->accepted_fence.configuration_fence_id !=
          evidence.configuration_fence_id ||
      acceptance->accepted_fence.configuration_fence_generation !=
          evidence.configuration_fence_generation ||
      acceptance->accepted_fence.captured_scope != evidence.reconciled_scope ||
      publication->publication.publication_receipt.request
              .candidate_projection_revision !=
          evidence.active_projection_revision ||
      publication->publication.publication_receipt.publication_generation !=
          evidence.active_publication_generation ||
      publication->publication.publication_receipt
              .durable_public_pointer_content_digest !=
          evidence.active_public_pointer_digest) {
    throw ManagerApplyJournalConflictError(
        "fence-release intent does not match exact completed, fence, and "
        "publication evidence");
  }
  if (const auto existing = impl_->FenceReleaseIntentLocked(
          owner.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->fence_release_intent != fence_release_intent ||
        existing->fence_release_intent_digest.value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has a different fence-release intent");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto count =
      ScalarInteger(impl_->database_,
                    "SELECT COUNT(*) FROM manager_apply_fence_release_intents");
  if (count >=
      static_cast<std::int64_t>(kMaximumManagerApplyFenceReleaseIntents)) {
    throw ManagerApplyJournalLimitError(
        "fence-release intent inventory reached its fixed limit");
  }
  Statement insert(impl_->database_,
                   "INSERT INTO manager_apply_fence_release_intents ("
                   "apply_id,apply_generation,scene_completion_digest,"
                   "fence_acceptance_digest,release_request_encoding,"
                   "fence_release_intent_digest) VALUES (?,?,?,?,?,?)");
  insert.Bind(1, owner.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, fence_release_intent.scene_completion_digest.value());
  insert.Bind(4, fence_release_intent.fence_acceptance_digest.value());
  insert.Bind(5, encoded.release_request);
  insert.Bind(6, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored =
      impl_->FenceReleaseIntentLocked(owner.apply_id, encoded.apply_generation);
  if (!stored.has_value() ||
      stored->fence_release_intent != fence_release_intent ||
      stored->fence_release_intent_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted fence-release intent cannot be verified");
  }
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyFenceReleaseIntent>
SqliteManagerApplyJournal::ReadFenceReleaseIntent(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->FenceReleaseIntentLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyFenceReleasedWriteResult
SqliteManagerApplyJournal::EnsureFenceReleased(
    const ManagerApplyFenceReleasedDTO &fence_released) {
  const auto encoded = EncodeFenceReleasedRecord(fence_released);
  const auto &owner = fence_released.released_fence.request
                          .reconciliation_evidence.owner_transaction;
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto intent =
      impl_->FenceReleaseIntentLocked(owner.apply_id, encoded.apply_generation);
  if (!intent.has_value() ||
      intent->fence_release_intent_digest !=
          fence_released.fence_release_intent_digest ||
      intent->fence_release_intent.release_request !=
          fence_released.released_fence.request) {
    throw ManagerApplyJournalConflictError(
        "fence release receipt does not match its exact durable intent");
  }
  if (const auto existing =
          impl_->FenceReleasedLocked(owner.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->fence_released != fence_released ||
        existing->fence_released_digest.value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has a different fence release receipt");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto count = ScalarInteger(
      impl_->database_,
      "SELECT COUNT(*) FROM manager_apply_fence_released_records");
  if (count >=
      static_cast<std::int64_t>(kMaximumManagerApplyFenceReleasedRecords)) {
    throw ManagerApplyJournalLimitError(
        "fence-released inventory reached its fixed limit");
  }
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_fence_released_records ("
      "apply_id,apply_generation,fence_release_intent_digest,"
      "released_fence_encoding,fence_released_digest) VALUES (?,?,?,?,?)");
  insert.Bind(1, owner.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, fence_released.fence_release_intent_digest.value());
  insert.Bind(4, encoded.released_fence);
  insert.Bind(5, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored =
      impl_->FenceReleasedLocked(owner.apply_id, encoded.apply_generation);
  if (!stored.has_value() || stored->fence_released != fence_released ||
      stored->fence_released_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted fence release receipt cannot be verified");
  }
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyFenceReleased>
SqliteManagerApplyJournal::ReadFenceReleased(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->FenceReleasedLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyTerminalOutcomeWriteResult
SqliteManagerApplyJournal::EnsureTerminalOutcome(
    const ManagerApplyTerminalOutcomeDTO &terminal_outcome) {
  const auto encoded = EncodeTerminalOutcome(terminal_outcome);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const bool succeeded =
      terminal_outcome.state == ManagerApplyTerminalOutcomeState::kSucceeded;
  if (succeeded) {
    const auto released = impl_->FenceReleasedLocked(terminal_outcome.apply_id,
                                                     encoded.apply_generation);
    const auto publication = impl_->PublicationReceiptLocked(
        terminal_outcome.apply_id, encoded.apply_generation);
    if (!released.has_value() ||
        released->fence_released_digest !=
            terminal_outcome.fence_released_digest ||
        !publication.has_value() ||
        publication->publication_receipt_digest !=
            terminal_outcome.scene_completion_digest ||
        publication->publication.publication_receipt.request
                .candidate_projection_revision !=
            terminal_outcome.candidate_projection_revision ||
        publication->publication_receipt_digest !=
            terminal_outcome.publication_receipt_digest) {
      throw ManagerApplyJournalConflictError(
          "terminal Apply outcome does not match exact completion evidence");
    }
  } else {
    // A destructive failure is retained against the exact accepted intent and
    // is never repaired, retried, rolled back or repaired from current state.
    const auto intent = impl_->IntentLocked(terminal_outcome.apply_id,
                                            encoded.apply_generation);
    if (!intent.has_value() ||
        intent->intent_digest != terminal_outcome.failure->intent_digest) {
      throw ManagerApplyJournalConflictError(
          "terminal Apply failure does not match its exact durable intent");
    }
  }
  if (const auto existing = impl_->TerminalOutcomeLocked(
          terminal_outcome.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->terminal_outcome != terminal_outcome ||
        existing->terminal_outcome_digest.value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has a different terminal outcome");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto count = ScalarInteger(
      impl_->database_, "SELECT COUNT(*) FROM manager_apply_terminal_outcomes");
  if (count >=
      static_cast<std::int64_t>(kMaximumManagerApplyTerminalOutcomes)) {
    throw ManagerApplyJournalLimitError(
        "terminal Apply outcome inventory reached its fixed limit");
  }
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_terminal_outcomes ("
      "apply_id,apply_generation,fence_released_digest,"
      "scene_completion_digest,publication_receipt_digest,"
      "candidate_retained_bundle_id,candidate_graph_revision,"
      "candidate_scene_id,candidate_scene_revision_id,"
      "candidate_scene_content_hash,intent_digest,failure_phase,failure_code,"
      "failure_message,effect_disposition,"
      "outcome_state,terminal_outcome_digest,cause_code,cause_details,"
      "producer,effect_boundary) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, terminal_outcome.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  if (succeeded) {
    const auto &candidate = *terminal_outcome.candidate_projection_revision;
    insert.Bind(3, terminal_outcome.fence_released_digest->value());
    insert.Bind(4, terminal_outcome.scene_completion_digest->value());
    insert.Bind(5, terminal_outcome.publication_receipt_digest->value());
    insert.Bind(6, candidate.retained_bundle_id.value());
    insert.Bind(7, candidate.graph_revision.value());
    insert.Bind(8, candidate.scene_revision.scene_id.value());
    insert.Bind(9, *encoded.candidate_scene_revision_id);
    insert.Bind(10, candidate.scene_revision.content_hash.value());
    insert.BindNull(11);
    insert.BindNull(12);
    insert.BindNull(13);
    insert.BindNull(14);
    insert.BindNull(15);
    insert.BindNull(18);
    insert.BindNull(19);
    insert.BindNull(20);
    insert.BindNull(21);
  } else {
    const auto &failure = *terminal_outcome.failure;
    insert.BindNull(3);
    insert.BindNull(4);
    insert.BindNull(5);
    insert.BindNull(6);
    insert.BindNull(7);
    insert.BindNull(8);
    insert.BindNull(9);
    insert.BindNull(10);
    insert.Bind(11, failure.intent_digest.value());
    insert.Bind(12, ToString(failure.phase));
    insert.Bind(13, failure.failure_code);
    insert.Bind(14, failure.failure_message);
    insert.Bind(15, ToString(failure.effect_disposition));
    insert.Bind(18, failure.cause_code);
    insert.Bind(19, EncodeCauseDetails(failure.cause_details));
    insert.Bind(20, failure.producer);
    insert.Bind(21, ToString(failure.effect_boundary));
  }
  insert.Bind(16, encoded.state);
  insert.Bind(17, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->TerminalOutcomeLocked(terminal_outcome.apply_id,
                                                   encoded.apply_generation);
  if (!stored.has_value() || stored->terminal_outcome != terminal_outcome ||
      stored->terminal_outcome_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted terminal Apply outcome cannot be verified");
  }
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyTerminalOutcome>
SqliteManagerApplyJournal::ReadTerminalOutcome(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->TerminalOutcomeLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

void SqliteManagerApplyJournal::BeginFailedApplyRelease(
    const ApplyId &apply_id, const ApplyGeneration apply_generation,
    const RequestId &acknowledgement_request_id,
    const std::string &acknowledgement_source_label,
    const ConfigurationFenceReleaseDTO &request) {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  ValidateBoundedIdentity(acknowledgement_request_id.value(), "request_id");
  ValidateBoundedText(acknowledgement_source_label, kMaximumManagerApplySourceLabelBytes,
                      "source_label");
  const auto generation = CheckedSigned(apply_generation.value(), "apply_generation");
  const auto encoded = EncodeFenceReleaseRequest(request);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto status = impl_->StatusLocked(apply_id, generation);
  if (!status || !HasFailedApplyReleaseEvidence(*status) ||
      impl_->FailedApplyReleaseLocked(apply_id, generation))
    throw ManagerApplyJournalConflictError("failed Apply cannot start another release attempt");
  Statement insert(impl_->database_,
                   "INSERT INTO manager_apply_failed_release "
                   "(apply_id,apply_generation,terminal_outcome_digest,fence_acceptance_digest,"
                   "acknowledgement_request_id,acknowledgement_source_label,"
                   "release_request_encoding,released_fence_encoding) VALUES (?,?,?,?,?,?,?,NULL)");
  insert.Bind(1, apply_id.value());
  insert.Bind(2, generation);
  insert.Bind(3, status->terminal_outcome->terminal_outcome_digest.value());
  insert.Bind(4, status->fence_acceptance->acceptance_digest.value());
  insert.Bind(5, acknowledgement_request_id.value());
  insert.Bind(6, acknowledgement_source_label);
  insert.Bind(7, encoded);
  static_cast<void>(insert.Step());
  static_cast<void>(impl_->FailedApplyReleaseLocked(apply_id, generation));
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
}

void SqliteManagerApplyJournal::CompleteFailedApplyRelease(
    const ApplyId &apply_id, const ApplyGeneration apply_generation,
    const ConfigurationFenceReleasedDTO &released) {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation = CheckedSigned(apply_generation.value(), "apply_generation");
  const auto encoded = EncodeFenceReleased(released);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto existing = impl_->FailedApplyReleaseLocked(apply_id, generation);
  if (!existing || existing->request != released.request)
    throw ManagerApplyJournalConflictError("failed Apply release receipt does not match its attempt");
  if (existing->released) {
    if (*existing->released != released)
      throw ManagerApplyJournalConflictError("failed Apply release already has another receipt");
    transaction.Commit();
    return;
  }
  Statement update(impl_->database_,
                   "UPDATE manager_apply_failed_release SET released_fence_encoding=? "
                   "WHERE apply_id=? AND apply_generation=? AND released_fence_encoding IS NULL");
  update.Bind(1, encoded);
  update.Bind(2, apply_id.value());
  update.Bind(3, generation);
  static_cast<void>(update.Step());
  if (sqlite3_changes(impl_->database_) != 1)
    throw ManagerApplyJournalConflictError("failed Apply release completion changed concurrently");
  static_cast<void>(impl_->FailedApplyReleaseLocked(apply_id, generation));
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
}

void SqliteManagerApplyJournal::CompleteFailedApplyAcknowledgement(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation = CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  Impl::WriteVerificationScope write_verification(*impl_);
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  const auto existing = impl_->FailedApplyReleaseLocked(apply_id, generation);
  if (!existing || !existing->released)
    throw ManagerApplyJournalConflictError("failed Apply has no completed fence release");
  if (existing->acknowledged) {
    transaction.Commit();
    return;
  }
  Statement update(impl_->database_,
                   "UPDATE manager_apply_failed_release SET acknowledged=1 "
                   "WHERE apply_id=? AND apply_generation=? AND acknowledged=0 "
                   "AND released_fence_encoding IS NOT NULL");
  update.Bind(1, apply_id.value());
  update.Bind(2, generation);
  static_cast<void>(update.Step());
  if (sqlite3_changes(impl_->database_) != 1)
    throw ManagerApplyJournalConflictError("failed Apply acknowledgement changed concurrently");
  static_cast<void>(impl_->FailedApplyReleaseLocked(apply_id, generation));
  static_cast<void>(impl_->VerifyWriteLimitsLocked());
  transaction.Commit();
}

std::optional<PersistedFailedApplyRelease>
SqliteManagerApplyJournal::ReadFailedApplyRelease(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation = CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto result = impl_->FailedApplyReleaseLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyJournalIntegrityReport
SqliteManagerApplyJournal::VerifyIntegrity() const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto report = impl_->VerifyIntegrityLocked();
  transaction.Commit();
  return report;
}

void SqliteManagerApplyJournal::Close() { impl_->Close(); }

namespace {

core::Result<void> WithReadOnlyApplySnapshot(const std::filesystem::path &path,
                                             const core::CellId &cell_id,
                                             auto &&inspect) {
  sqlite3 *database = nullptr;
  try {
    PreflightApplyJournal(path, cell_id);
    const auto opened = sqlite3_open_v2(
        path.string().c_str(), &database,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
        nullptr);
    Check(database, opened, "open read-only configuration Apply proof");
    Check(database, sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds),
          "configure read-only configuration Apply timeout");
    Exec(database, "PRAGMA query_only=ON");
    {
      Transaction snapshot(database, "BEGIN");
      ValidateApplyJournalIdentityAndSchema(database, cell_id, path);
      inspect(database);
      snapshot.Commit();
    }
    sqlite3_close(database);
    return core::Result<void>::Ok();
  } catch (const std::exception &error) {
    if (database)
      sqlite3_close(database);
    return core::Result<void>::Err(core::ErrorDTO{
        "configuration_apply_proof_unavailable",
        "exact Manager Apply proof is unavailable",
        {{"reason", std::string(error.what()).substr(0U, 512U)}}});
  }
}

void RequireExactManagerApplyClaim(const core::CellAdmissionClaimDTO &claim,
                                   const PersistedManagerApplyStatus &status) {
  if (!core::ValidateCellAdmissionClaimIntent(claim.intent).ok() ||
      !claim.claim_generation.nonzero() || claim.accepted_at_ns < 0 ||
      !status.intent.intent.expected_admission_generation ||
      status.intent.intent.parent_authoring_claim) {
    throw ManagerApplyJournalIntegrityError(
        "configuration_apply_claim_invalid");
  }
  const core::CellAdmissionClaimIntentDTO expected{
      core::CellAdmissionClaimKindDTO::kManagerApply,
      core::CellAdmissionOwnerRoleDTO::kManager,
      status.intent.intent.request_context.request_owner_epoch,
      status.intent.intent.apply_id.value(),
      status.intent.intent.request_context.request_id,
      status.intent.intent_digest,
      *status.intent.intent.expected_admission_generation,
      {},
      std::nullopt,
      std::nullopt,
      std::nullopt};
  if (claim.intent != expected)
    throw ManagerApplyJournalIntegrityError(
        "configuration_apply_claim_identity_mismatch");
}

} // namespace

core::Result<void> ValidateTerminalManagerApplyClaimsReadOnly(
    const std::filesystem::path &database_path, const core::CellId &cell_id,
    const std::span<const core::CellAdmissionClaimDTO> old_claims) {
  if (old_claims.size() > core::kMaximumCellAdmissionClaims ||
      !std::is_sorted(old_claims.begin(), old_claims.end(),
                      [](const auto &a, const auto &b) {
                        return a.claim_generation < b.claim_generation;
                      }) ||
      std::adjacent_find(old_claims.begin(), old_claims.end(),
                         [](const auto &a, const auto &b) {
                           return a.claim_generation == b.claim_generation;
                         }) != old_claims.end()) {
    return core::Result<void>::Err(
        core::ErrorDTO{"invalid_request",
                       "old Apply claim inventory is not bounded and canonical",
                       {}});
  }
  return WithReadOnlyApplySnapshot(
      database_path, cell_id, [&](sqlite3 *database) {
        SqliteManagerApplyJournal::Impl view(database, cell_id);
        for (const auto &claim : old_claims) {
          const auto intent = view.RequestLocked(claim.intent.owner_epoch,
                                                 claim.intent.request_id);
          if (!intent)
            throw ManagerApplyJournalIntegrityError(
                "configuration_apply_original_intent_missing");
          const auto status = view.StatusWithFailedReleaseLocked(
              intent->intent.apply_id,
              CheckedSigned(intent->intent.apply_generation.value(),
                            "apply_generation"));
          if (!status)
            throw ManagerApplyJournalIntegrityError(
                "configuration_apply_original_status_missing");
          RequireExactManagerApplyClaim(claim, *status);
          if (!status->terminal_outcome)
            throw ManagerApplyJournalIntegrityError(
                "configuration_apply_holder_live");
          if (status->terminal_outcome->terminal_outcome.succeeded())
            throw ManagerApplyJournalIntegrityError(
                "configuration_apply_holder_requires_success_accounting");
        }
      });
}

core::Result<void> ValidateFreshManagerApplySuccessReadOnly(
    const std::filesystem::path &database_path, const core::CellId &cell_id,
    const core::CellAdmissionClaimDTO &fresh_claim,
    const core::CellAdmissionClaimReleaseDTO &release) {
  if (!core::ValidateCellAdmissionClaimRelease(release).ok() ||
      release !=
          core::CellAdmissionClaimReleaseDTO{
              fresh_claim.intent.owner_role, fresh_claim.intent.owner_epoch,
              fresh_claim.intent.owner_transaction_id,
              fresh_claim.intent.request_id,
              fresh_claim.intent.request_payload_digest,
              fresh_claim.acquired_admission_generation,
              fresh_claim.claim_generation, release.settlement} ||
      release.settlement.settlement_kind !=
          core::CellAdmissionSettlementKindDTO::kTerminalEffectsSettled) {
    return core::Result<void>::Err(core::ErrorDTO{
        "invalid_request", "fresh Apply success release identity differs", {}});
  }
  return WithReadOnlyApplySnapshot(
      database_path, cell_id, [&](sqlite3 *database) {
        SqliteManagerApplyJournal::Impl view(database, cell_id);
        const auto intent = view.RequestLocked(fresh_claim.intent.owner_epoch,
                                               fresh_claim.intent.request_id);
        if (!intent)
          throw ManagerApplyJournalIntegrityError(
              "fresh_apply_original_intent_missing");
        const auto status = view.StatusWithFailedReleaseLocked(
            intent->intent.apply_id,
            CheckedSigned(intent->intent.apply_generation.value(),
                          "apply_generation"));
        if (!status)
          throw ManagerApplyJournalIntegrityError(
              "fresh_apply_original_status_missing");
        RequireExactManagerApplyClaim(fresh_claim, *status);
        if (!status->terminal_outcome ||
            !status->terminal_outcome->terminal_outcome.succeeded() ||
            status->terminal_outcome->terminal_outcome_digest !=
                release.settlement.evidence_digest) {
          throw ManagerApplyJournalIntegrityError(
              "fresh_apply_success_terminal_not_exact");
        }
      });
}

core::Result<void> ValidateManagerApplyCellAdmissionSettlementReadOnly(
    const std::filesystem::path &database_path, const core::CellId &cell_id,
    const core::CellAdmissionClaimDTO &claim,
    const core::CellAdmissionClaimReleaseDTO &release) {
  const auto rejected = [](std::string reason) {
    return core::Result<void>::Err(core::ErrorDTO{
        "invariant_violation",
        "Manager Apply cell-admission settlement is not owner-proven",
        {{"reason", std::move(reason)}}});
  };
  if (!core::ValidateCellAdmissionClaimIntent(claim.intent).ok() ||
      claim.intent.claim_kind != core::CellAdmissionClaimKindDTO::kManagerApply ||
      claim.intent.owner_role != core::CellAdmissionOwnerRoleDTO::kManager ||
      release.owner_role != claim.intent.owner_role ||
      release.owner_epoch != claim.intent.owner_epoch ||
      release.owner_transaction_id != claim.intent.owner_transaction_id ||
      release.request_id != claim.intent.request_id ||
      release.request_payload_digest != claim.intent.request_payload_digest ||
      release.acquired_admission_generation !=
          claim.acquired_admission_generation ||
      release.claim_generation != claim.claim_generation ||
      release.settlement.settlement_kind !=
          core::CellAdmissionSettlementKindDTO::kTerminalEffectsSettled) {
    return rejected("manager_apply_release_identity_mismatch");
  }

  sqlite3 *database = nullptr;
  const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX |
                    SQLITE_OPEN_NOFOLLOW;
  const auto opened = sqlite3_open_v2(database_path.string().c_str(), &database,
                                      flags, nullptr);
  if (opened != SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return rejected("manager_apply_journal_unavailable");
  }
  try {
    Check(database, sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds),
          "configure read-only settlement timeout");
    Exec(database, "PRAGMA query_only=ON");
    std::optional<std::string> digest;
    bool duplicate = false;
    {
      Transaction snapshot(database, "BEGIN");
      ValidateApplyJournalIdentityAndSchema(database, cell_id, database_path);
      SqliteManagerApplyJournal::Impl view(database, cell_id);
      Statement row(
          database,
          "SELECT t.terminal_outcome_digest,i.apply_generation,t.outcome_state,"
          "t.failure_phase FROM manager_apply_intents i "
          "JOIN manager_apply_terminal_outcomes t "
          "ON t.apply_id=i.apply_id AND "
          "t.apply_generation=i.apply_generation "
          // A completed release (no-effect acknowledgement, or New for any
          // failure, N7) also settles a held failed Apply. Its recorded
          // request and receipt are verified below, not by this existence
          // check alone.
          "AND (t.outcome_state='succeeded' OR "
          "(t.outcome_state='failed' AND t.effect_disposition='known' "
          "AND t.failure_phase='accepted') OR "
          "(t.outcome_state<>'succeeded' AND EXISTS "
          "(SELECT 1 FROM manager_apply_failed_release f "
          "WHERE f.apply_id=t.apply_id AND f.apply_generation=t.apply_generation "
          "AND f.released_fence_encoding IS NOT NULL))) "
          "WHERE i.apply_id=? AND i.request_owner_epoch=? AND i.request_id=? "
          "AND i.intent_digest=? AND i.expected_admission_generation=? "
          "AND i.parent_authoring_claim IS NULL");
      row.Bind(1, claim.intent.owner_transaction_id);
      row.Bind(2, claim.intent.owner_epoch.value());
      row.Bind(3, claim.intent.request_id.value());
      row.Bind(4, claim.intent.request_payload_digest.value());
      row.Bind(5, CheckedSigned(claim.intent.expected_admission_generation.value(),
                                "expected_admission_generation"));
      const bool found = row.Step();
      digest = found ? std::optional<std::string>(row.Text(0)) : std::nullopt;
      if (found && row.Text(2) != "succeeded" &&
          !(row.Text(2) == "failed" && row.Text(3) == "accepted")) {
        const auto acknowledged = view.FailedApplyReleaseLocked(
            ApplyId(claim.intent.owner_transaction_id), row.Integer(1));
        if (!acknowledged || !acknowledged->released ||
            acknowledged->terminal_outcome_digest.value() != *digest)
          throw ManagerApplyJournalIntegrityError(
              "failed Apply has no completed release");
      }
      duplicate = found && row.Step();
      snapshot.Commit();
    }
    const auto close_result = sqlite3_close(database);
    if (close_result != SQLITE_OK)
      ThrowSqlite(database, close_result, "close read-only settlement database");
    database = nullptr;
    if (!digest.has_value() || duplicate ||
        *digest != release.settlement.evidence_digest.value()) {
      return rejected("manager_apply_terminal_evidence_mismatch");
    }
    return core::Result<void>::Ok();
  } catch (const std::exception &) {
    if (database != nullptr) sqlite3_close(database);
    return rejected("manager_apply_journal_read_failed");
  }
}

core::Result<PersistedManagerApplyStatus> ReadManagerApplyStatusReadOnlyForTesting(
    const std::filesystem::path &database_path, const CellId &cell_id,
    const core::NamedApplyPublicationTransactionDTO &apply,
    std::uint64_t *lookup_count) {
  using Reply = core::Result<PersistedManagerApplyStatus>;
  const auto rejected = [] {
    return Reply::Err({"manager_apply_read_unavailable",
                       "The exact retained Apply cannot be verified read-only",
                       {{"reason", "manager_apply_read_invalid"}}});
  };
  if (!apply.valid())
    return rejected();
  sqlite3 *raw = nullptr;
  try {
    PreflightApplyJournal(database_path, cell_id);
    if (sqlite3_open_v2(database_path.string().c_str(), &raw,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX |
                            SQLITE_OPEN_NOFOLLOW,
                        nullptr) != SQLITE_OK) {
      if (raw)
        sqlite3_close(raw);
      raw = nullptr;
      return rejected();
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw,
                                                                sqlite3_close);
    raw = nullptr;
    Check(database.get(),
          sqlite3_busy_timeout(database.get(), kBusyTimeoutMilliseconds),
          "configure read-only Apply timeout");
    Exec(database.get(), "PRAGMA query_only=ON");
    Transaction snapshot(database.get(), "BEGIN");
    ValidateApplyJournalIdentityAndSchema(database.get(), cell_id,
                                          database_path);
    SqliteManagerApplyJournal::Impl view(database.get(), cell_id);
    auto status = view.StatusWithFailedReleaseLocked(
        apply.apply_id,
        CheckedSigned(apply.apply_generation.value(), "apply_generation"));
    if (!status)
      return rejected();
    snapshot.Commit();
    if (lookup_count != nullptr)
      *lookup_count = view.record_verifications_;
    return Reply::Ok(std::move(*status));
  } catch (const std::exception &) {
    if (raw)
      sqlite3_close(raw);
    return rejected();
  }
}

core::Result<PersistedManagerApplyStatus> ReadManagerApplyStatusReadOnly(
    const std::filesystem::path &database_path, const CellId &cell_id,
    const core::NamedApplyPublicationTransactionDTO &apply) {
  return ReadManagerApplyStatusReadOnlyForTesting(database_path, cell_id,
                                                   apply, nullptr);
}

core::Result<PersistedManagerApplyStatus> ReadManagerApplyStatusByClaimIntentReadOnly(
    const std::filesystem::path &database_path, const CellId &cell_id,
    const core::CellAdmissionClaimIntentDTO &parent_claim_intent) {
  const auto rejected = [](std::string reason) {
    return core::Result<PersistedManagerApplyStatus>::Err(core::ErrorDTO{
        "manager_apply_configuration_read_unavailable",
        "the exact retained configuration Apply parent cannot be observed",
        {{"reason", std::move(reason)}}});
  };
  if (!core::ValidateCellAdmissionClaimIntent(parent_claim_intent).ok() ||
      parent_claim_intent.claim_kind != core::CellAdmissionClaimKindDTO::kManagerApply ||
      parent_claim_intent.owner_role != core::CellAdmissionOwnerRoleDTO::kManager)
    return rejected("parent_claim_intent_invalid");
  try {
    PreflightApplyJournal(database_path, cell_id);
  } catch (const std::exception &) {
    return rejected("parent_journal_preflight_failed");
  }
  sqlite3 *database = nullptr;
  const auto opened = sqlite3_open_v2(
      database_path.string().c_str(), &database,
      SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
      nullptr);
  if (opened != SQLITE_OK) {
    if (database != nullptr) sqlite3_close(database);
    return rejected("parent_journal_unavailable");
  }
  try {
    Check(database, sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds),
          "configure read-only parent timeout");
    Exec(database, "PRAGMA query_only=ON");
    std::optional<PersistedManagerApplyStatus> result;
    {
      Transaction snapshot(database, "BEGIN");
      ValidateApplyJournalIdentityAndSchema(database, cell_id, database_path);
      SqliteManagerApplyJournal::Impl view(database, cell_id);
      const auto intent = view.RequestLocked(parent_claim_intent.owner_epoch,
                                             parent_claim_intent.request_id);
      if (!intent ||
          intent->intent.apply_id.value() != parent_claim_intent.owner_transaction_id ||
          intent->intent.expected_admission_generation !=
              std::optional(parent_claim_intent.expected_admission_generation) ||
          intent->intent.parent_authoring_claim)
        throw ManagerApplyJournalIntegrityError("retained parent claim intent mismatch");
      result = view.StatusWithFailedReleaseLocked(intent->intent.apply_id,
          CheckedSigned(intent->intent.apply_generation.value(), "apply_generation"));
      if (!result)
        throw ManagerApplyJournalIntegrityError("retained parent status missing");
      const core::CellAdmissionClaimIntentDTO expected{
          core::CellAdmissionClaimKindDTO::kManagerApply,
          core::CellAdmissionOwnerRoleDTO::kManager,
          intent->intent.request_context.request_owner_epoch,
          intent->intent.apply_id.value(),
          intent->intent.request_context.request_id,
          result->intent.intent_digest,
          *intent->intent.expected_admission_generation,
          {}, std::nullopt, std::nullopt, std::nullopt};
      if (parent_claim_intent != expected)
        throw ManagerApplyJournalIntegrityError("retained original parent claim mismatch");
      snapshot.Commit();
    }
    sqlite3_close(database);
    database = nullptr;
    return core::Result<PersistedManagerApplyStatus>::Ok(std::move(*result));
  } catch (const std::exception &) {
    if (database != nullptr) sqlite3_close(database);
    return rejected("parent_journal_or_identity_invalid");
  }
}

core::Result<PersistedManagerApplyStatus> ReadManagerApplyConfigurationStatusReadOnly(
    const std::filesystem::path &database_path, const CellId &cell_id,
    const core::NamedApplyPublicationTransactionDTO &apply,
    const core::CellAdmissionClaimDTO &actual_parent_claim) {
  using ReadResult = core::Result<PersistedManagerApplyStatus>;
  const auto rejected = [](std::string reason, std::string detail = {}) {
    auto error = core::ErrorDTO{
        "manager_apply_configuration_read_unavailable",
        "the exact retained configuration Apply parent cannot be observed",
        {{"reason", std::move(reason)}}};
    if (!detail.empty()) error.details.emplace("integrity_detail", std::move(detail));
    return ReadResult::Err(std::move(error));
  };
  const core::CellAdmissionRecoveryReceiptQueryDTO claim_query{
      actual_parent_claim.intent, actual_parent_claim, std::nullopt};
  if (!apply.valid() || !core::ValidateCellAdmissionRecoveryReceiptQuery(claim_query).ok())
    return rejected("configuration_parent_query_invalid");
  try {
    PreflightApplyJournal(database_path, cell_id);
  } catch (const std::exception &) {
    return rejected("parent_journal_preflight_failed");
  }
  sqlite3 *database = nullptr;
  const auto opened = sqlite3_open_v2(
      database_path.string().c_str(), &database,
      SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr);
  if (opened != SQLITE_OK) {
    if (database != nullptr) sqlite3_close(database);
    return rejected("parent_journal_unavailable");
  }
  try {
    Check(database, sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds),
          "configure read-only configuration parent timeout");
    Exec(database, "PRAGMA query_only=ON");
    std::optional<PersistedManagerApplyStatus> result;
    {
      Transaction snapshot(database, "BEGIN");
      ValidateApplyJournalIdentityAndSchema(database, cell_id, database_path);
      SqliteManagerApplyJournal::Impl view(database, cell_id);
      result = view.StatusWithFailedReleaseLocked(apply.apply_id,
          CheckedSigned(apply.apply_generation.value(), "apply_generation"));
      if (!result)
        throw ManagerApplyJournalIntegrityError("ordinary configuration parent missing");
      if (result->intent.intent.cell_id != cell_id)
        throw ManagerApplyJournalIntegrityError("configuration parent cell mismatch");
      const auto &intent = result->intent.intent;
      if (intent.parent_authoring_claim) {
        if (intent.expected_admission_generation ||
            actual_parent_claim.intent.claim_kind != core::CellAdmissionClaimKindDTO::kAuthoringMutation ||
            actual_parent_claim.intent.owner_role != core::CellAdmissionOwnerRoleDTO::kAuthoringOwner ||
            *intent.parent_authoring_claim != actual_parent_claim)
          throw ManagerApplyJournalIntegrityError("configuration authoring parent claim mismatch");
      } else {
        if (!intent.expected_admission_generation)
          throw ManagerApplyJournalIntegrityError("configuration direct admission generation missing");
        const core::CellAdmissionClaimIntentDTO expected{
            core::CellAdmissionClaimKindDTO::kManagerApply,
            core::CellAdmissionOwnerRoleDTO::kManager,
            intent.request_context.request_owner_epoch, intent.apply_id.value(),
            intent.request_context.request_id, result->intent.intent_digest,
            *intent.expected_admission_generation, {}, std::nullopt, std::nullopt, std::nullopt};
        const auto requested = view.RequestLocked(expected.owner_epoch, expected.request_id);
        if (actual_parent_claim.intent != expected || !requested || *requested != result->intent)
          throw ManagerApplyJournalIntegrityError("configuration direct parent intent mismatch");
      }
      snapshot.Commit();
    }
    sqlite3_close(database);
    database = nullptr;
    return ReadResult::Ok(std::move(*result));
  } catch (const ManagerApplyJournalIntegrityError &error) {
    if (database != nullptr) sqlite3_close(database);
    // Only this reader's fixed diagnostic literals may cross the host wire.
    // Legacy integrity exceptions can include source paths or nested errors.
    const std::string_view detail(error.what());
    if (detail == "ordinary configuration parent missing" ||
        detail == "configuration parent cell mismatch" ||
        detail == "configuration authoring parent claim mismatch" ||
        detail == "configuration direct admission generation missing" ||
        detail == "configuration direct parent intent mismatch") {
      return rejected("parent_journal_or_identity_invalid", std::string(detail.substr(0, 160U)));
    }
    return rejected("parent_journal_or_identity_invalid");
  } catch (const std::exception &) {
    if (database != nullptr) sqlite3_close(database);
    return rejected("parent_journal_or_identity_invalid");
  }
}

core::Result<core::CellAdmissionReconcileValidationDTO>
ValidateManagerAuthoringOwnerReconciliationReadOnly(
    const std::filesystem::path &database_path, const core::CellId &cell_id,
    const std::vector<core::CellAdmissionClaimDTO> &held_claims,
    const std::vector<core::CellAdmissionCurrentOwnerEpochDTO>
        &current_owner_epochs,
    const core::CellAdmissionReconcileDTO &reconcile) {
  const auto rejected = [](std::string reason) {
    return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
        core::ErrorDTO{
            "invariant_violation",
            "Composer authoring owner reconciliation is not proven",
            {{"reason", std::move(reason)}}});
  };
  constexpr auto role = core::CellAdmissionOwnerRoleDTO::kAuthoringOwner;
  if (held_claims != reconcile.retained_claims) {
    return rejected("authoring_reconciliation_inventory_mismatch");
  }
  const auto predecessor = std::find_if(
      current_owner_epochs.begin(), current_owner_epochs.end(),
      [](const auto &owner) { return owner.owner_role == role; });
  const auto successor = std::find_if(
      reconcile.current_owner_epochs.begin(),
      reconcile.current_owner_epochs.end(),
      [](const auto &owner) { return owner.owner_role == role; });
  if (successor == reconcile.current_owner_epochs.end() ||
      successor->owner_epoch.empty() ||
      (predecessor != current_owner_epochs.end() &&
       predecessor->owner_epoch == successor->owner_epoch)) {
    return rejected("authoring_reconciliation_successor_invalid");
  }
  for (const auto &owner : current_owner_epochs) {
    if (owner.owner_role == role) {
      continue;
    }
    if (std::find(reconcile.current_owner_epochs.begin(),
                  reconcile.current_owner_epochs.end(), owner) ==
        reconcile.current_owner_epochs.end()) {
      return rejected("authoring_reconciliation_changes_unrelated_owner");
    }
  }
  for (const auto &owner : reconcile.current_owner_epochs) {
    if (owner.owner_role == role) {
      continue;
    }
    if (std::find(current_owner_epochs.begin(), current_owner_epochs.end(),
                  owner) == current_owner_epochs.end()) {
      return rejected("authoring_reconciliation_changes_unrelated_owner");
    }
  }
  const auto expected_evidence =
      core::CellAdmissionOwnerEpochReconciliationEvidenceDigest(
          role, successor->owner_epoch);
  if (!expected_evidence.ok() ||
      reconcile.reconciliation_evidence_digest != expected_evidence.value()) {
    return rejected("authoring_reconciliation_evidence_mismatch");
  }

  sqlite3 *database = nullptr;
  const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX |
                    SQLITE_OPEN_NOFOLLOW;
  if (sqlite3_open_v2(database_path.string().c_str(), &database, flags,
                      nullptr) != SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return rejected("manager_apply_journal_unavailable");
  }
  try {
    Check(database, sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds),
          "configure authoring reconciliation timeout");
    Exec(database, "PRAGMA query_only=ON");
    Transaction snapshot(database, "BEGIN");
    ValidateApplyJournalIdentityAndSchema(database, cell_id, database_path);
    core::CellAdmissionReconcileValidationDTO validation{
        reconcile.current_owner_epochs, {}};
    for (const auto &claim : held_claims) {
      if (claim.intent.owner_role != role ||
          (predecessor != current_owner_epochs.end() &&
           claim.intent.owner_epoch != predecessor->owner_epoch)) {
        continue;
      }
      const auto encoded_claim = EncodeParentAuthoringClaim(claim);
      Statement operation(
          database,
          "SELECT t.outcome_state,t.effect_disposition,t.failure_phase "
          "FROM manager_apply_intents i "
          "LEFT JOIN manager_apply_terminal_outcomes t "
          "ON t.apply_id=i.apply_id AND "
          "t.apply_generation=i.apply_generation "
          "WHERE i.parent_authoring_claim=?");
      operation.BindBlob(1, encoded_claim);
      const bool transferred = operation.Step();
      const bool terminal_known =
          transferred && !operation.IsNull(0) &&
          (operation.Text(0) == "succeeded" ||
           (operation.Text(0) == "failed" && !operation.IsNull(1) &&
            operation.Text(1) == "known" && !operation.IsNull(2) &&
            operation.Text(2) == "accepted"));
      const bool duplicate = transferred && operation.Step();
      if (duplicate || (transferred && !terminal_known)) {
        throw std::runtime_error(
            "accepted Manager operation is not known terminal");
      }
      validation.settlements.push_back(
          {claim, role, successor->owner_epoch,
           core::CellAdmissionSettlementKindDTO::kReconciled,
           reconcile.reconciliation_evidence_digest});
    }
    snapshot.Commit();
    sqlite3_close(database);
    return core::Result<core::CellAdmissionReconcileValidationDTO>::Ok(
        std::move(validation));
  } catch (const std::exception &) {
    sqlite3_close(database);
    return rejected("authoring_reconciliation_manager_evidence_invalid");
  }
}

} // namespace manyforge::component_manager
