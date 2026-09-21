#include "manyforge_component_manager/manager_apply_journal.hpp"

#include <manyforge_deployment/apply_fence_qualification.hpp>

#include <openssl/evp.h>
#include <sqlite3.h>

#include <manyforge_persistence/sqlite_read_only_preflight.hpp>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>

#include "embedded_manager_apply_journal_schema.hpp"
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
using core::NamedApplyProjectionSetSourceDTO;
using core::NamedApplyPublicationTransactionDTO;
using core::ProductContractId;
using core::ProductContractRevision;
using core::ProductId;
using core::ProjectionAggregateHealthDTO;
using core::ProjectionApplyActivationResultDTO;
using core::ProjectionApplyActivationResultState;
using core::ProjectionApplyFinalizeResultDTO;
using core::ProjectionApplyFinalizeResultState;
using core::ProjectionApplyStageResultDTO;
using core::ProjectionApplyStageResultState;
using core::ProjectionHealthDTO;
using core::ProjectionHealthState;
using core::ProjectionNativeRuntimeBindingDTO;
using core::ProjectionRevisionDTO;
using core::ProjectionStagedCandidateDTO;
using core::ProjectionStageGeneration;
using core::ProjectionTransitionDisposition;
using core::ProjectionTransitionEvidenceDTO;
using core::ProjectionTransitionEvidenceState;
using core::ProjectionTransitionRowDTO;
using core::ProjectionTransitionRowEvidenceDTO;
using core::ProjectionTransitionRowEvidenceState;
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
    "manyforge.component-manager-apply-intent.v1";
constexpr std::string_view kAffectedScopeDigestDomain =
    "manyforge.component-manager-apply-affected-scope.v1";
constexpr std::string_view kFenceAttemptDigestDomain =
    "manyforge.component-manager-apply-fence-attempt.v2";
constexpr std::string_view kFenceAcceptanceDigestDomain =
    "manyforge.component-manager-apply-fence-acceptance.v2";
constexpr std::string_view kRuntimeConvergenceIntentDigestDomain =
    "manyforge.component-manager-runtime-convergence-intent.v2";
constexpr std::string_view kRuntimeStagedReadyDigestDomain =
    "manyforge.component-manager-runtime-staged-ready.v3";
constexpr std::string_view kSceneStageIntentDigestDomain =
    "manyforge.component-manager-scene-stage-intent.v2";
constexpr std::string_view kSceneStagedEvidenceDigestDomain =
    "manyforge.component-manager-scene-staged-evidence.v2";
constexpr std::string_view kActivationSelectionDigestDomain =
    "manyforge.component-manager-activation-selection.v1";
constexpr std::string_view kSceneActivatedEvidenceDigestDomain =
    "manyforge.component-manager-scene-activated-evidence.v2";
constexpr std::string_view kPrivateMaterializationDigestDomain =
    "manyforge.component-manager-private-selected-bundle.v1";
constexpr std::string_view kRetiredCommandSurfacesDigestDomain =
    "manyforge.component-manager-obsolete-command-surfaces-retired.v1";
constexpr std::string_view kPublicationReceiptDigestDomain =
    "manyforge.component-manager-apply-publication-receipt.v1";
constexpr std::string_view kSceneFinalizedEvidenceDigestDomain =
    "manyforge.component-manager-scene-finalized-evidence.v2";
constexpr std::string_view kFenceReleaseIntentDigestDomain =
    "manyforge.component-manager-fence-release-intent.v1";
constexpr std::string_view kFenceReleasedDigestDomain =
    "manyforge.component-manager-fence-released.v1";
constexpr std::string_view kTerminalOutcomeDigestDomain =
    "manyforge.component-manager-terminal-outcome.v1";
constexpr int kBusyTimeoutMilliseconds = 10000;
constexpr std::int64_t kWalCheckpointTargetBytes = 16LL * 1024LL * 1024LL;
constexpr std::int64_t kExpectedSchemaObjectCardinality = 54;

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

void Exec(sqlite3 *database, const std::string_view sql) {
  char *error = nullptr;
  const auto result = sqlite3_exec(database, std::string(sql).c_str(), nullptr,
                                   nullptr, &error);
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

std::string
EncodeDomains(const std::vector<ConfigurationFenceDomainScopeDTO> &domains) {
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
      EncodeDomains(evidence.reconciled_scope.domains),
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
  ValidateDigest(intent.scene_finalized_digest.value(),
                 "scene_finalized_digest");
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
  AppendField(record, "scene_finalized_digest",
              intent.scene_finalized_digest.value());
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
  std::int64_t candidate_scene_revision_id;
  std::string state;
  std::string digest;
};

EncodedTerminalOutcome
EncodeTerminalOutcome(const ManagerApplyTerminalOutcomeDTO &outcome) {
  if (!outcome.valid()) {
    throw std::invalid_argument("Manager terminal Apply outcome is invalid");
  }
  ValidateBoundedIdentity(outcome.apply_id.value(), "apply_id");
  ValidateDigest(outcome.publication_receipt_digest.value(),
                 "publication_receipt_digest");
  ValidateDigest(outcome.scene_finalized_digest.value(),
                 "scene_finalized_digest");
  ValidateDigest(outcome.fence_released_digest.value(),
                 "fence_released_digest");
  const auto &candidate = outcome.candidate_projection_revision;
  EncodedTerminalOutcome encoded{
      CheckedSigned(outcome.apply_generation.value(), "apply_generation"),
      CheckedSigned(candidate.scene_revision.revision_id,
                    "candidate_scene_revision_id"),
      ToString(outcome.state),
      {},
  };
  std::string record;
  AppendField(record, "domain", kTerminalOutcomeDigestDomain);
  AppendField(record, "fence_released_digest",
              outcome.fence_released_digest.value());
  AppendField(record, "scene_finalized_digest",
              outcome.scene_finalized_digest.value());
  AppendField(record, "publication_receipt_digest",
              outcome.publication_receipt_digest.value());
  AppendField(record, "apply_id", outcome.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
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
  AppendField(record, "outcome_state", encoded.state);
  encoded.digest = RawDigest(record);
  return encoded;
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
      EncodeDomains(affected_scope.scope.domains),
      EncodeBindings(affected_scope.scope.bindings),
      EncodeResources(affected_scope.scope.resources),
      std::string(core::to_string(affected_scope.busy_disposition)),
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
      EncodeDomains(intent.scope.domains),
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
      EncodeDomains(accepted.captured_scope.domains),
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

std::string EncodeManagedWithdrawal(
    const std::optional<core::ManagedRuntimeWithdrawalDTO> &receipt) {
  if (!receipt)
    return {};
  if (!receipt->valid())
    throw std::invalid_argument("invalid managed withdrawal");
  return EncodeTokens({receipt->apply_id.value(),
                       std::to_string(receipt->apply_generation.value()),
                       receipt->runtime_convergence_intent_digest.value(),
                       receipt->runtime_unit_id.value(),
                       receipt->runtime_epoch.value(),
                       receipt->product_identity.value(),
                       receipt->configuration_identity.value(),
                       receipt->process_observer_instance_id,
                       std::to_string(receipt->process_observation_sequence)});
}

std::optional<core::ManagedRuntimeWithdrawalDTO>
DecodeManagedWithdrawal(const std::string &encoding) {
  if (encoding.empty())
    return std::nullopt;
  const auto receipt =
      DecodeTokens(encoding, 9U, "managed predecessor withdrawal");
  if (receipt.size() != 9U)
    throw ManagerApplyJournalIntegrityError(
        "stored withdrawal cardinality is invalid");
  core::ManagedRuntimeWithdrawalDTO result{
      ApplyId(receipt[0]),
      ApplyGeneration(
          ParseCanonicalUnsigned(receipt[1], "withdrawal Apply generation")),
      ContentDigest(receipt[2]),
      RuntimeUnitId(receipt[3]),
      RuntimeEpoch(receipt[4]),
      ContentDigest(receipt[5]),
      ContentDigest(receipt[6]),
      receipt[7],
      ParseCanonicalUnsigned(receipt[8], "withdrawal observation sequence")};
  if (!result.valid())
    throw ManagerApplyJournalIntegrityError(
        "stored managed withdrawal is invalid");
  return result;
}

std::string EncodeRuntimeStagedReadyRows(
    const std::vector<RuntimeUnitStagedReadyEvidenceDTO> &runtime_units) {
  std::vector<std::string> tokens;
  tokens.reserve(runtime_units.size() * 20U);
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
    tokens.push_back(EncodeManagedWithdrawal(row.predecessor_withdrawal));
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
  constexpr std::size_t kFieldsPerRow = 20U;
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
    row.predecessor_withdrawal = DecodeManagedWithdrawal(tokens[index + 19U]);
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

void AppendProjectionTokens(std::vector<std::string> &tokens,
                            const ProjectionRevisionDTO &projection) {
  if (!projection.valid()) {
    throw std::invalid_argument("scene-stage projection is invalid");
  }
  ValidateBoundedIdentity(projection.retained_bundle_id.value(),
                          "projection retained_bundle_id");
  ValidateBoundedIdentity(projection.graph_revision.value(),
                          "projection graph_revision");
  ValidateDigest(projection.scene_revision.scene_id.value(),
                 "projection scene_id");
  ValidateDigest(projection.scene_revision.content_hash.value(),
                 "projection scene_content_hash");
  tokens.push_back(projection.retained_bundle_id.value());
  tokens.push_back(projection.graph_revision.value());
  tokens.push_back(projection.scene_revision.scene_id.value());
  tokens.push_back(std::to_string(projection.scene_revision.revision_id));
  tokens.push_back(projection.scene_revision.content_hash.value());
}

void AppendOptionalProjectionTokens(
    std::vector<std::string> &tokens,
    const std::optional<ProjectionRevisionDTO> &projection) {
  tokens.emplace_back(projection.has_value() ? "1" : "0");
  if (projection.has_value()) {
    AppendProjectionTokens(tokens, *projection);
    return;
  }
  for (std::size_t index = 0U; index < 5U; ++index) {
    tokens.emplace_back();
  }
}

ProjectionRevisionDTO
DecodeProjectionTokens(const std::vector<std::string> &tokens,
                       const std::size_t begin, const std::string_view label) {
  ProjectionRevisionDTO projection{
      RetainedBundleId(tokens.at(begin)),
      GraphRevision(tokens.at(begin + 1U)),
      SceneRevision{
          SceneId(tokens.at(begin + 2U)),
          ParseCanonicalUnsigned(tokens.at(begin + 3U), label),
          ContentDigest(tokens.at(begin + 4U)),
      },
  };
  if (!projection.valid()) {
    throw ManagerApplyJournalIntegrityError(
        "stored scene-stage projection is invalid");
  }
  return projection;
}

std::optional<ProjectionRevisionDTO>
DecodeOptionalProjectionTokens(const std::vector<std::string> &tokens,
                               const std::size_t presence,
                               const std::string_view label) {
  if (tokens.at(presence) == "1") {
    return DecodeProjectionTokens(tokens, presence + 1U, label);
  }
  if (tokens.at(presence) != "0" ||
      std::any_of(tokens.begin() + static_cast<std::ptrdiff_t>(presence + 1U),
                  tokens.begin() + static_cast<std::ptrdiff_t>(presence + 6U),
                  [](const std::string &value) { return !value.empty(); })) {
    throw ManagerApplyJournalIntegrityError(
        "stored optional scene-stage projection has invalid presence");
  }
  return std::nullopt;
}

ProjectionTransitionDisposition
DecodeTransitionDisposition(const std::string_view value) {
  if (value == "retained") {
    return ProjectionTransitionDisposition::kRetained;
  }
  if (value == "added") {
    return ProjectionTransitionDisposition::kAdded;
  }
  if (value == "removed") {
    return ProjectionTransitionDisposition::kRemoved;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored projection transition disposition is invalid");
}

void AppendTransitionRowTokens(std::vector<std::string> &tokens,
                               const ProjectionTransitionRowDTO &row) {
  if (!row.valid()) {
    throw std::invalid_argument("scene transition row is invalid");
  }
  const auto &binding = row.native_runtime_binding;
  ValidateBoundedIdentity(binding.provider_binding_id.value(),
                          "projection provider_binding_id");
  ValidateBoundedIdentity(binding.provider_binding_revision.value(),
                          "projection provider_binding_revision");
  ValidateBoundedIdentity(binding.runtime_unit_id.value(),
                          "projection runtime_unit_id");
  ValidateBoundedIdentity(binding.runtime_epoch.value(),
                          "projection runtime_epoch");
  tokens.push_back(binding.provider_binding_id.value());
  tokens.push_back(binding.provider_binding_revision.value());
  tokens.push_back(binding.runtime_unit_id.value());
  tokens.push_back(binding.runtime_epoch.value());
  tokens.emplace_back(core::to_string(row.disposition));
  AppendOptionalProjectionTokens(tokens, row.expected_local_active);
  tokens.push_back(EncodeManagedWithdrawal(row.managed_withdrawal));
}

ProjectionTransitionRowDTO
DecodeTransitionRowTokens(const std::vector<std::string> &tokens,
                          const std::size_t begin) {
  ProjectionTransitionRowDTO row{
      ProjectionNativeRuntimeBindingDTO{
          core::CapabilityProviderBindingId(tokens.at(begin)),
          core::CapabilityProviderBindingRevision(tokens.at(begin + 1U)),
          RuntimeUnitId(tokens.at(begin + 2U)),
          RuntimeEpoch(tokens.at(begin + 3U)),
      },
      DecodeTransitionDisposition(tokens.at(begin + 4U)),
      DecodeOptionalProjectionTokens(tokens, begin + 5U,
                                     "transition expected active revision"),
      DecodeManagedWithdrawal(tokens.at(begin + 11U)),
  };
  if (!row.valid()) {
    throw ManagerApplyJournalIntegrityError(
        "stored projection transition row is invalid");
  }
  return row;
}

std::string
EncodeTransitionRows(const std::vector<ProjectionTransitionRowDTO> &rows) {
  if (rows.size() > kMaximumManagerApplySceneTransitionRows ||
      !core::CanonicalProjectionTransitionRows(rows)) {
    throw std::invalid_argument(
        "scene transition rows are noncanonical or exceed their bound");
  }
  std::vector<std::string> tokens;
  tokens.reserve(rows.size() * 12U);
  for (const auto &row : rows) {
    AppendTransitionRowTokens(tokens, row);
  }
  return EncodeTokens(tokens);
}

std::vector<ProjectionTransitionRowDTO>
DecodeTransitionRows(const std::string &encoding) {
  constexpr std::size_t kFieldsPerRow = 12U;
  const auto tokens = DecodeTokens(
      encoding, kMaximumManagerApplySceneTransitionRows * kFieldsPerRow,
      "scene transition rows");
  if (tokens.size() % kFieldsPerRow != 0U) {
    throw ManagerApplyJournalIntegrityError(
        "stored scene transition tuple cardinality is invalid");
  }
  std::vector<ProjectionTransitionRowDTO> rows;
  rows.reserve(tokens.size() / kFieldsPerRow);
  for (std::size_t index = 0U; index < tokens.size(); index += kFieldsPerRow) {
    rows.push_back(DecodeTransitionRowTokens(tokens, index));
  }
  if (!core::CanonicalProjectionTransitionRows(rows)) {
    throw ManagerApplyJournalIntegrityError(
        "stored scene transition rows are not canonical");
  }
  return rows;
}

ProjectionTransitionRowEvidenceState
DecodeTransitionRowEvidenceState(const std::string_view value) {
  if (value == "pending") {
    return ProjectionTransitionRowEvidenceState::kPending;
  }
  if (value == "staged_no_active_effect") {
    return ProjectionTransitionRowEvidenceState::kStagedNoActiveEffect;
  }
  if (value == "candidate_active") {
    return ProjectionTransitionRowEvidenceState::kCandidateActive;
  }
  if (value == "removed_retired") {
    return ProjectionTransitionRowEvidenceState::kRemovedRetired;
  }
  if (value == "candidate_stage_discarded") {
    return ProjectionTransitionRowEvidenceState::kCandidateStageDiscarded;
  }
  if (value == "predecessor_unchanged") {
    return ProjectionTransitionRowEvidenceState::kPredecessorUnchanged;
  }
  if (value == "uncertain") {
    return ProjectionTransitionRowEvidenceState::kUncertain;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored transition-row evidence state is invalid");
}

ProjectionHealthState
DecodeProjectionHealthState(const std::string_view value) {
  if (value == "uninitialized") {
    return ProjectionHealthState::kUninitialized;
  }
  if (value == "healthy") {
    return ProjectionHealthState::kHealthy;
  }
  if (value == "desynchronized") {
    return ProjectionHealthState::kDesynchronized;
  }
  if (value == "rebuilding") {
    return ProjectionHealthState::kRebuilding;
  }
  if (value == "unavailable") {
    return ProjectionHealthState::kUnavailable;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored projection health state is invalid");
}

void AppendOptionalStagedCandidateTokens(
    std::vector<std::string> &tokens,
    const std::optional<ProjectionStagedCandidateDTO> &staged) {
  tokens.emplace_back(staged.has_value() ? "1" : "0");
  if (!staged.has_value()) {
    for (std::size_t index = 0U; index < 8U; ++index) {
      tokens.emplace_back();
    }
    return;
  }
  if (!staged->valid()) {
    throw std::invalid_argument("projection staged candidate is invalid");
  }
  tokens.push_back(staged->apply_id.value());
  tokens.push_back(std::to_string(staged->apply_generation.value()));
  tokens.push_back(std::to_string(staged->projection_stage_generation.value()));
  AppendProjectionTokens(tokens, staged->candidate_projection_revision);
}

std::optional<ProjectionStagedCandidateDTO>
DecodeOptionalStagedCandidateTokens(const std::vector<std::string> &tokens,
                                    const std::size_t presence) {
  if (tokens.at(presence) == "0") {
    if (std::any_of(tokens.begin() + static_cast<std::ptrdiff_t>(presence + 1U),
                    tokens.begin() + static_cast<std::ptrdiff_t>(presence + 9U),
                    [](const std::string &value) { return !value.empty(); })) {
      throw ManagerApplyJournalIntegrityError(
          "stored staged-candidate absence carries fields");
    }
    return std::nullopt;
  }
  if (tokens.at(presence) != "1") {
    throw ManagerApplyJournalIntegrityError(
        "stored staged-candidate presence is invalid");
  }
  ProjectionStagedCandidateDTO staged{
      ApplyId(tokens.at(presence + 1U)),
      ApplyGeneration(ParseCanonicalUnsigned(tokens.at(presence + 2U),
                                             "staged Apply generation")),
      ProjectionStageGeneration(ParseCanonicalUnsigned(
          tokens.at(presence + 3U), "projection stage generation")),
      DecodeProjectionTokens(tokens, presence + 4U,
                             "staged candidate scene revision"),
  };
  if (!staged.valid()) {
    throw ManagerApplyJournalIntegrityError(
        "stored staged candidate is invalid");
  }
  return staged;
}

void AppendOptionalUnsignedTokens(std::vector<std::string> &tokens,
                                  const std::optional<std::uint64_t> value) {
  tokens.emplace_back(value.has_value() ? "1" : "0");
  tokens.push_back(value.has_value() ? std::to_string(*value) : std::string{});
}

std::optional<std::uint64_t>
DecodeOptionalUnsignedTokens(const std::vector<std::string> &tokens,
                             const std::size_t presence,
                             const std::string_view label) {
  if (tokens.at(presence) == "1") {
    const auto value = ParseCanonicalUnsigned(tokens.at(presence + 1U), label);
    if (value == 0U) {
      throw ManagerApplyJournalIntegrityError(
          "stored optional unsigned value is zero");
    }
    return value;
  }
  if (tokens.at(presence) != "0" || !tokens.at(presence + 1U).empty()) {
    throw ManagerApplyJournalIntegrityError(
        "stored optional unsigned presence is invalid");
  }
  return std::nullopt;
}

void AppendOptionalDigestTokens(std::vector<std::string> &tokens,
                                const std::optional<ContentDigest> &digest) {
  tokens.emplace_back(digest.has_value() ? "1" : "0");
  tokens.push_back(digest.has_value() ? digest->value() : std::string{});
}

std::optional<ContentDigest>
DecodeOptionalDigestTokens(const std::vector<std::string> &tokens,
                           const std::size_t presence) {
  if (tokens.at(presence) == "1") {
    ValidateDigest(tokens.at(presence + 1U), "projection content digest");
    return ContentDigest(tokens.at(presence + 1U));
  }
  if (tokens.at(presence) != "0" || !tokens.at(presence + 1U).empty()) {
    throw ManagerApplyJournalIntegrityError(
        "stored optional digest presence is invalid");
  }
  return std::nullopt;
}

bool WithdrawalsMatchRuntimeStage(
    const ManagerApplySceneStageIntentDTO &intent,
    const ManagerApplyRuntimeStagedReadyDTO &ready) {
  for (const auto &row : intent.native_transition_rows) {
    if (!row.managed_withdrawal)
      continue;
    const auto found =
        std::find_if(ready.runtime_units.begin(), ready.runtime_units.end(),
                     [&](const auto &runtime) {
                       return runtime.runtime_unit_id ==
                              row.native_runtime_binding.runtime_unit_id;
                     });
    if (found == ready.runtime_units.end() ||
        found->predecessor_withdrawal != row.managed_withdrawal)
      return false;
  }
  return true;
}

std::string EncodeTransitionEvidenceRows(
    const std::vector<ProjectionTransitionRowEvidenceDTO> &rows) {
  if (rows.size() > kMaximumManagerApplySceneTransitionRows) {
    throw ManagerApplyJournalLimitError(
        "scene staged evidence exceeds its transition-row bound");
  }
  std::vector<std::string> tokens;
  tokens.reserve(rows.size() * 46U);
  for (const auto &row : rows) {
    if (!row.valid() || row.error.has_value() ||
        (row.resulting_health && row.resulting_health->error.has_value())) {
      throw std::invalid_argument(
          "successful scene staged row carries invalid or error evidence");
    }
    AppendTransitionRowTokens(tokens, row.transition_row);
    tokens.emplace_back(core::to_string(row.state));
    AppendOptionalProjectionTokens(tokens,
                                   row.acknowledged_staged_candidate_revision);
    AppendOptionalProjectionTokens(tokens, row.observed_local_active);
    if (!row.resulting_health) {
      tokens.insert(tokens.end(), 21U, std::string{});
      continue;
    }
    tokens.push_back(std::to_string(row.resulting_health->source_sequence));
    tokens.emplace_back(core::to_string(row.resulting_health->state));
    AppendOptionalProjectionTokens(
        tokens, row.resulting_health->active_projection_revision);
    AppendOptionalStagedCandidateTokens(tokens,
                                        row.resulting_health->staged_candidate);
    AppendOptionalUnsignedTokens(
        tokens, row.resulting_health->native_projection_generation);
    AppendOptionalDigestTokens(tokens,
                               row.resulting_health->projection_content_digest);
  }
  return EncodeTokens(tokens);
}

std::vector<ProjectionTransitionRowEvidenceDTO>
DecodeTransitionEvidenceRows(const std::string &encoding) {
  constexpr std::size_t kFieldsPerRow = 46U;
  const auto tokens = DecodeTokens(
      encoding, kMaximumManagerApplySceneTransitionRows * kFieldsPerRow,
      "scene staged transition evidence");
  if (tokens.size() % kFieldsPerRow != 0U) {
    throw ManagerApplyJournalIntegrityError(
        "stored scene staged-evidence tuple cardinality is invalid");
  }
  std::vector<ProjectionTransitionRowEvidenceDTO> rows;
  rows.reserve(tokens.size() / kFieldsPerRow);
  for (std::size_t index = 0U; index < tokens.size(); index += kFieldsPerRow) {
    auto transition_row = DecodeTransitionRowTokens(tokens, index);
    std::optional<ProjectionHealthDTO> health;
    if (tokens[index + 25U].empty()) {
      if (std::any_of(tokens.begin() + index + 25U,
                      tokens.begin() + index + kFieldsPerRow,
                      [](const auto &value) { return !value.empty(); }))
        throw ManagerApplyJournalIntegrityError(
            "absent native health has payload");
    } else {
      health = ProjectionHealthDTO{
          transition_row.native_runtime_binding,
          ParseCanonicalUnsigned(tokens[index + 25U],
                                 "projection health source sequence"),
          DecodeProjectionHealthState(tokens[index + 26U]),
          DecodeOptionalProjectionTokens(tokens, index + 27U,
                                         "health active projection revision"),
          DecodeOptionalStagedCandidateTokens(tokens, index + 33U),
          DecodeOptionalUnsignedTokens(tokens, index + 42U,
                                       "native projection generation"),
          DecodeOptionalDigestTokens(tokens, index + 44U),
          std::nullopt,
      };
    }
    ProjectionTransitionRowEvidenceDTO row{
        std::move(transition_row),
        DecodeTransitionRowEvidenceState(tokens[index + 12U]),
        DecodeOptionalProjectionTokens(tokens, index + 13U,
                                       "staged candidate acknowledgement"),
        DecodeOptionalProjectionTokens(tokens, index + 19U,
                                       "observed local active projection"),
        std::move(health),
        std::nullopt,
    };
    if (!row.valid()) {
      throw ManagerApplyJournalIntegrityError(
          "stored scene staged transition row is invalid");
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string EncodeTransitionEvidence(
    const ProjectionTransitionEvidenceDTO &evidence,
    const ProjectionTransitionEvidenceState expected_state) {
  if (!evidence.phase_consistent(expected_state) ||
      evidence.error.has_value() ||
      evidence.transition_rows.size() >
          kMaximumManagerApplySceneTransitionRows) {
    throw std::invalid_argument("scene staged transition evidence is invalid");
  }
  std::vector<std::string> tokens;
  tokens.reserve(15U);
  tokens.push_back(evidence.apply_id.value());
  tokens.push_back(std::to_string(evidence.apply_generation.value()));
  tokens.push_back(
      std::to_string(evidence.projection_stage_generation.value()));
  AppendProjectionTokens(tokens, evidence.predecessor_projection_revision);
  AppendProjectionTokens(tokens, evidence.candidate_projection_revision);
  tokens.emplace_back(core::to_string(evidence.state));
  tokens.push_back(EncodeTransitionEvidenceRows(evidence.transition_rows));
  return EncodeTokens(tokens);
}

ProjectionTransitionEvidenceDTO DecodeTransitionEvidence(
    const std::string &encoding,
    const ProjectionTransitionEvidenceState expected_state) {
  const auto tokens = DecodeTokens(encoding, 15U, "scene staged evidence");
  if (tokens.size() != 15U || tokens[13U] != core::to_string(expected_state)) {
    throw ManagerApplyJournalIntegrityError(
        "stored scene staged evidence header is invalid");
  }
  ProjectionTransitionEvidenceDTO evidence{
      ApplyId(tokens[0]),
      ApplyGeneration(
          ParseCanonicalUnsigned(tokens[1], "scene staged Apply generation")),
      ProjectionStageGeneration(ParseCanonicalUnsigned(
          tokens[2], "scene staged projection generation")),
      DecodeProjectionTokens(tokens, 3U, "scene staged predecessor revision"),
      DecodeProjectionTokens(tokens, 8U, "scene staged candidate revision"),
      expected_state,
      DecodeTransitionEvidenceRows(tokens[14U]),
      std::nullopt,
  };
  if (!evidence.phase_consistent(expected_state)) {
    throw ManagerApplyJournalIntegrityError(
        "stored scene staged evidence is phase-inconsistent");
  }
  return evidence;
}

struct EncodedSceneStageIntent final {
  std::int64_t apply_generation;
  std::int64_t predecessor_publication_generation;
  std::int64_t predecessor_scene_revision_id;
  std::string transition_rows;
  std::string digest;
};

EncodedSceneStageIntent
EncodeSceneStageIntent(const ManagerApplySceneStageIntentDTO &intent) {
  if (!intent.valid()) {
    throw std::invalid_argument("scene-stage intent is invalid");
  }
  EncodedSceneStageIntent encoded{
      CheckedSigned(intent.apply_generation.value(), "apply_generation"),
      CheckedSigned(intent.predecessor_publication_generation.value(),
                    "predecessor_publication_generation"),
      CheckedSigned(
          intent.expected_active_projection_revision.scene_revision.revision_id,
          "predecessor_scene_revision_id"),
      EncodeTransitionRows(intent.native_transition_rows),
      {},
  };
  std::string record;
  AppendField(record, "domain", kSceneStageIntentDigestDomain);
  AppendField(record, "runtime_staged_ready_digest",
              intent.runtime_staged_ready_digest.value());
  AppendField(record, "apply_id", intent.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "predecessor_publication_generation",
              std::to_string(encoded.predecessor_publication_generation));
  AppendField(record, "predecessor_public_pointer_digest",
              intent.predecessor_public_pointer_digest.value());
  const auto &predecessor = intent.expected_active_projection_revision;
  AppendField(record, "predecessor_retained_bundle_id",
              predecessor.retained_bundle_id.value());
  AppendField(record, "predecessor_graph_revision",
              predecessor.graph_revision.value());
  AppendField(record, "predecessor_scene_id",
              predecessor.scene_revision.scene_id.value());
  AppendField(record, "predecessor_scene_revision_id",
              std::to_string(encoded.predecessor_scene_revision_id));
  AppendField(record, "predecessor_scene_content_hash",
              predecessor.scene_revision.content_hash.value());
  AppendField(record, "candidate_retained_bundle_id",
              intent.candidate_retained_bundle_id.value());
  AppendField(record, "candidate_graph_revision",
              intent.candidate_graph_revision.value());
  AppendField(record, "native_transition_rows_encoding",
              encoded.transition_rows);
  encoded.digest = RawDigest(record);
  return encoded;
}

struct EncodedSceneStaged final {
  std::int64_t apply_generation;
  std::int64_t predecessor_scene_revision_id;
  std::int64_t projection_stage_generation;
  std::int64_t candidate_scene_revision_id;
  std::string transition_evidence;
  std::string digest;
};

EncodedSceneStaged EncodeSceneStaged(const ManagerApplySceneStagedDTO &staged) {
  if (!staged.valid()) {
    throw std::invalid_argument("scene staged evidence is invalid");
  }
  const auto &result = staged.staged_result;
  const auto &predecessor = result.expected_active_projection_revision;
  const auto &candidate = *result.candidate_projection_revision;
  EncodedSceneStaged encoded{
      CheckedSigned(result.apply_generation.value(), "apply_generation"),
      CheckedSigned(predecessor.scene_revision.revision_id,
                    "predecessor_scene_revision_id"),
      CheckedSigned(result.projection_stage_generation->value(),
                    "projection_stage_generation"),
      CheckedSigned(candidate.scene_revision.revision_id,
                    "candidate_scene_revision_id"),
      EncodeTransitionEvidence(*result.transition_evidence,
                               ProjectionTransitionEvidenceState::kStaged),
      {},
  };
  std::string record;
  AppendField(record, "domain", kSceneStagedEvidenceDigestDomain);
  AppendField(record, "scene_stage_intent_digest",
              staged.scene_stage_intent_digest.value());
  AppendField(record, "apply_id", result.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "predecessor_retained_bundle_id",
              predecessor.retained_bundle_id.value());
  AppendField(record, "predecessor_graph_revision",
              predecessor.graph_revision.value());
  AppendField(record, "predecessor_scene_id",
              predecessor.scene_revision.scene_id.value());
  AppendField(record, "predecessor_scene_revision_id",
              std::to_string(encoded.predecessor_scene_revision_id));
  AppendField(record, "predecessor_scene_content_hash",
              predecessor.scene_revision.content_hash.value());
  AppendField(record, "projection_stage_generation",
              std::to_string(encoded.projection_stage_generation));
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
  AppendField(record, "result_state", "staged");
  AppendField(record, "transition_evidence_encoding",
              encoded.transition_evidence);
  encoded.digest = RawDigest(record);
  return encoded;
}

struct EncodedActivationSelection final {
  std::int64_t apply_generation;
  std::int64_t predecessor_scene_revision_id;
  std::int64_t projection_stage_generation;
  std::int64_t candidate_scene_revision_id;
  std::string digest;
};

EncodedActivationSelection
EncodeActivationSelection(const ManagerApplyActivationSelectionDTO &selection) {
  if (!selection.valid()) {
    throw std::invalid_argument("activation selection is invalid");
  }
  EncodedActivationSelection encoded{
      CheckedSigned(selection.apply_generation.value(), "apply_generation"),
      CheckedSigned(
          selection.predecessor_projection_revision.scene_revision.revision_id,
          "predecessor_scene_revision_id"),
      CheckedSigned(selection.projection_stage_generation.value(),
                    "projection_stage_generation"),
      CheckedSigned(
          selection.candidate_projection_revision.scene_revision.revision_id,
          "candidate_scene_revision_id"),
      {},
  };
  std::string record;
  AppendField(record, "domain", kActivationSelectionDigestDomain);
  AppendField(record, "scene_staged_digest",
              selection.scene_staged_digest.value());
  AppendField(record, "apply_id", selection.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  const auto &predecessor = selection.predecessor_projection_revision;
  AppendField(record, "predecessor_retained_bundle_id",
              predecessor.retained_bundle_id.value());
  AppendField(record, "predecessor_graph_revision",
              predecessor.graph_revision.value());
  AppendField(record, "predecessor_scene_id",
              predecessor.scene_revision.scene_id.value());
  AppendField(record, "predecessor_scene_revision_id",
              std::to_string(encoded.predecessor_scene_revision_id));
  AppendField(record, "predecessor_scene_content_hash",
              predecessor.scene_revision.content_hash.value());
  AppendField(record, "projection_stage_generation",
              std::to_string(encoded.projection_stage_generation));
  const auto &candidate = selection.candidate_projection_revision;
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
  encoded.digest = RawDigest(record);
  return encoded;
}

struct EncodedSceneActivated final {
  std::int64_t apply_generation;
  std::int64_t predecessor_scene_revision_id;
  std::int64_t projection_stage_generation;
  std::int64_t candidate_scene_revision_id;
  std::string transition_evidence;
  std::string digest;
};

EncodedSceneActivated
EncodeSceneActivated(const ManagerApplySceneActivatedDTO &activated) {
  if (!activated.valid()) {
    throw std::invalid_argument("scene activated evidence is invalid");
  }
  const auto &result = activated.activation_result;
  const auto &predecessor = result.expected_active_projection_revision;
  const auto &candidate = result.candidate_projection_revision;
  EncodedSceneActivated encoded{
      CheckedSigned(result.apply_generation.value(), "apply_generation"),
      CheckedSigned(predecessor.scene_revision.revision_id,
                    "predecessor_scene_revision_id"),
      CheckedSigned(result.projection_stage_generation.value(),
                    "projection_stage_generation"),
      CheckedSigned(candidate.scene_revision.revision_id,
                    "candidate_scene_revision_id"),
      EncodeTransitionEvidence(
          result.transition_evidence,
          ProjectionTransitionEvidenceState::kActivatedAndRetired),
      {},
  };
  std::string record;
  AppendField(record, "domain", kSceneActivatedEvidenceDigestDomain);
  AppendField(record, "activation_selection_digest",
              activated.activation_selection_digest.value());
  AppendField(record, "apply_id", result.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "predecessor_retained_bundle_id",
              predecessor.retained_bundle_id.value());
  AppendField(record, "predecessor_graph_revision",
              predecessor.graph_revision.value());
  AppendField(record, "predecessor_scene_id",
              predecessor.scene_revision.scene_id.value());
  AppendField(record, "predecessor_scene_revision_id",
              std::to_string(encoded.predecessor_scene_revision_id));
  AppendField(record, "predecessor_scene_content_hash",
              predecessor.scene_revision.content_hash.value());
  AppendField(record, "projection_stage_generation",
              std::to_string(encoded.projection_stage_generation));
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
  AppendField(record, "result_state", "activated");
  AppendField(record, "transition_evidence_encoding",
              encoded.transition_evidence);
  encoded.digest = RawDigest(record);
  return encoded;
}

struct EncodedPrivateMaterialization final {
  std::int64_t apply_generation;
  std::int64_t projection_stage_generation;
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
      CheckedSigned(materialization.projection_stage_generation.value(),
                    "projection_stage_generation"),
      CheckedSigned(materialization.candidate_projection_revision.scene_revision
                        .revision_id,
                    "candidate_scene_revision_id"),
      CheckedSigned(materialization.retained_bundle_canonical_byte_size,
                    "retained_bundle_canonical_byte_size"),
      {},
  };
  std::string record;
  AppendField(record, "domain", kPrivateMaterializationDigestDomain);
  AppendField(record, "scene_activated_digest",
              materialization.scene_activated_digest.value());
  AppendField(record, "apply_id", materialization.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "projection_stage_generation",
              std::to_string(encoded.projection_stage_generation));
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

ObsoleteManagedCommandSurfaceDisposition
DecodeObsoleteCommandSurfaceDisposition(const std::string_view value) {
  if (value == "retired") {
    return ObsoleteManagedCommandSurfaceDisposition::kRetired;
  }
  if (value == "already_absent") {
    return ObsoleteManagedCommandSurfaceDisposition::kAlreadyAbsent;
  }
  if (value == "nonselectable") {
    return ObsoleteManagedCommandSurfaceDisposition::kNonselectable;
  }
  throw ManagerApplyJournalIntegrityError(
      "stored obsolete command-surface disposition is invalid");
}

std::string EncodeObsoleteCommandSurfaces(
    const std::vector<ObsoleteManagedCommandSurfaceEvidenceDTO> &rows) {
  if (rows.size() > kMaximumObsoleteManagedCommandSurfaces) {
    throw ManagerApplyJournalLimitError(
        "obsolete command-surface evidence exceeds its fixed bound");
  }
  std::vector<std::string> tokens;
  tokens.reserve(rows.size() * 6U);
  for (const auto &row : rows) {
    if (!row.valid()) {
      throw std::invalid_argument(
          "obsolete command-surface evidence row is invalid");
    }
    ValidateBoundedIdentity(row.runtime_unit_id.value(), "runtime_unit_id");
    ValidateBoundedIdentity(row.runtime_epoch.value(), "runtime_epoch");
    ValidateDigest(row.product_identity.value(), "product_identity");
    ValidateDigest(row.configuration_identity.value(),
                   "configuration_identity");
    tokens.push_back(row.runtime_unit_id.value());
    tokens.push_back(row.runtime_epoch.value());
    tokens.push_back(row.product_identity.value());
    tokens.push_back(row.configuration_identity.value());
    tokens.push_back(std::to_string(row.source_report_sequence));
    tokens.emplace_back(ToString(row.disposition));
  }
  return EncodeTokens(tokens);
}

std::vector<ObsoleteManagedCommandSurfaceEvidenceDTO>
DecodeObsoleteCommandSurfaces(const std::string &encoding) {
  constexpr std::size_t kFieldsPerRow = 6U;
  const auto tokens = DecodeTokens(
      encoding, kMaximumObsoleteManagedCommandSurfaces * kFieldsPerRow,
      "obsolete command-surface evidence");
  if (tokens.size() % kFieldsPerRow != 0U) {
    throw ManagerApplyJournalIntegrityError(
        "stored obsolete command-surface tuple cardinality is invalid");
  }
  std::vector<ObsoleteManagedCommandSurfaceEvidenceDTO> rows;
  rows.reserve(tokens.size() / kFieldsPerRow);
  for (std::size_t index = 0U; index < tokens.size(); index += kFieldsPerRow) {
    ObsoleteManagedCommandSurfaceEvidenceDTO row{
        RuntimeUnitId(tokens[index]),
        RuntimeEpoch(tokens[index + 1U]),
        ContentDigest(tokens[index + 2U]),
        ContentDigest(tokens[index + 3U]),
        ParseCanonicalUnsigned(tokens[index + 4U], "source_report_sequence"),
        DecodeObsoleteCommandSurfaceDisposition(tokens[index + 5U]),
    };
    if (!row.valid()) {
      throw ManagerApplyJournalIntegrityError(
          "stored obsolete command-surface evidence row is invalid");
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

struct EncodedRetiredCommandSurfaces final {
  std::int64_t apply_generation;
  std::string command_surfaces;
  std::string digest;
};

EncodedRetiredCommandSurfaces EncodeRetiredCommandSurfaces(
    const ManagerApplyObsoleteCommandSurfacesRetiredDTO &retired) {
  if (!retired.valid()) {
    throw std::invalid_argument(
        "obsolete command-surface retirement evidence is invalid");
  }
  EncodedRetiredCommandSurfaces encoded{
      CheckedSigned(retired.apply_generation.value(), "apply_generation"),
      EncodeObsoleteCommandSurfaces(retired.command_surfaces),
      {},
  };
  std::string record;
  AppendField(record, "domain", kRetiredCommandSurfacesDigestDomain);
  AppendField(record, "private_materialization_content_digest",
              retired.private_materialization_content_digest.value());
  AppendField(record, "apply_id", retired.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "command_surfaces_encoding", encoded.command_surfaces);
  encoded.digest = RawDigest(record);
  return encoded;
}

struct EncodedPublicationReceipt final {
  std::int64_t apply_generation;
  std::int64_t projection_stage_generation;
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
      CheckedSigned(transaction->projection_stage_generation.value(),
                    "projection_stage_generation"),
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
  AppendField(
      record, "retired_command_surfaces_evidence_digest",
      publication.obsolete_managed_command_surfaces_retired_evidence_digest
          .value());
  AppendField(record, "apply_id", transaction->apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "projection_stage_generation",
              std::to_string(encoded.projection_stage_generation));
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
  AppendField(record, "private_materialization_content_digest",
              receipt.request.private_materialization_content_digest.value());
  AppendField(record, "publication_generation",
              std::to_string(encoded.publication_generation));
  AppendField(record, "durable_public_pointer_content_digest",
              receipt.durable_public_pointer_content_digest.value());
  encoded.digest = RawDigest(record);
  return encoded;
}

std::string
EncodeProjectionHealthRows(const std::vector<ProjectionHealthDTO> &rows) {
  if (rows.size() > kMaximumManagerApplySceneTransitionRows) {
    throw ManagerApplyJournalLimitError(
        "candidate aggregate health exceeds its fixed bound");
  }
  std::vector<std::string> tokens;
  tokens.reserve(rows.size() * 25U);
  for (const auto &health : rows) {
    if (!health.valid() || health.error.has_value()) {
      throw std::invalid_argument("candidate projection health is invalid");
    }
    const auto &binding = health.native_runtime_binding;
    tokens.push_back(binding.provider_binding_id.value());
    tokens.push_back(binding.provider_binding_revision.value());
    tokens.push_back(binding.runtime_unit_id.value());
    tokens.push_back(binding.runtime_epoch.value());
    tokens.push_back(std::to_string(health.source_sequence));
    tokens.emplace_back(core::to_string(health.state));
    AppendOptionalProjectionTokens(tokens, health.active_projection_revision);
    AppendOptionalStagedCandidateTokens(tokens, health.staged_candidate);
    AppendOptionalUnsignedTokens(tokens, health.native_projection_generation);
    AppendOptionalDigestTokens(tokens, health.projection_content_digest);
  }
  return EncodeTokens(tokens);
}

std::vector<ProjectionHealthDTO>
DecodeProjectionHealthRows(const std::string &encoding) {
  constexpr std::size_t kFieldsPerRow = 25U;
  const auto tokens = DecodeTokens(
      encoding, kMaximumManagerApplySceneTransitionRows * kFieldsPerRow,
      "candidate aggregate health");
  if (tokens.size() % kFieldsPerRow != 0U) {
    throw ManagerApplyJournalIntegrityError(
        "stored candidate-health tuple cardinality is invalid");
  }
  std::vector<ProjectionHealthDTO> rows;
  rows.reserve(tokens.size() / kFieldsPerRow);
  for (std::size_t index = 0U; index < tokens.size(); index += kFieldsPerRow) {
    ProjectionHealthDTO health{
        ProjectionNativeRuntimeBindingDTO{
            core::CapabilityProviderBindingId(tokens[index]),
            core::CapabilityProviderBindingRevision(tokens[index + 1U]),
            RuntimeUnitId(tokens[index + 2U]),
            RuntimeEpoch(tokens[index + 3U]),
        },
        ParseCanonicalUnsigned(tokens[index + 4U],
                               "projection health source sequence"),
        DecodeProjectionHealthState(tokens[index + 5U]),
        DecodeOptionalProjectionTokens(tokens, index + 6U,
                                       "health active projection revision"),
        DecodeOptionalStagedCandidateTokens(tokens, index + 12U),
        DecodeOptionalUnsignedTokens(tokens, index + 21U,
                                     "native projection generation"),
        DecodeOptionalDigestTokens(tokens, index + 23U),
        std::nullopt,
    };
    if (!health.valid()) {
      throw ManagerApplyJournalIntegrityError(
          "stored candidate projection health is invalid");
    }
    rows.push_back(std::move(health));
  }
  return rows;
}

std::string EncodeAggregateHealth(const ProjectionAggregateHealthDTO &health) {
  const auto *source = std::get_if<NamedApplyProjectionSetSourceDTO>(
      &health.required_native_set_source);
  if (source == nullptr || !health.valid() || health.error.has_value()) {
    throw std::invalid_argument(
        "named-Apply candidate aggregate health is invalid");
  }
  std::vector<std::string> tokens;
  tokens.reserve(10U);
  tokens.push_back(source->apply_id.value());
  tokens.push_back(std::to_string(source->apply_generation.value()));
  tokens.push_back(std::to_string(source->projection_stage_generation.value()));
  AppendProjectionTokens(tokens, source->candidate_projection_revision);
  tokens.emplace_back(core::to_string(health.state));
  tokens.push_back(EncodeProjectionHealthRows(health.native_projection_health));
  return EncodeTokens(tokens);
}

ProjectionAggregateHealthDTO
DecodeAggregateHealth(const std::string &encoding) {
  const auto tokens = DecodeTokens(encoding, 10U, "candidate aggregate health");
  if (tokens.size() != 10U) {
    throw ManagerApplyJournalIntegrityError(
        "stored candidate aggregate-health header is invalid");
  }
  ProjectionAggregateHealthDTO health{
      NamedApplyProjectionSetSourceDTO{
          ApplyId(tokens[0]),
          ApplyGeneration(
              ParseCanonicalUnsigned(tokens[1], "health Apply generation")),
          ProjectionStageGeneration(ParseCanonicalUnsigned(
              tokens[2], "health projection stage generation")),
          DecodeProjectionTokens(tokens, 3U, "health candidate revision"),
      },
      DecodeProjectionHealthState(tokens[8]),
      DecodeProjectionHealthRows(tokens[9]),
      std::nullopt,
  };
  if (!health.valid()) {
    throw ManagerApplyJournalIntegrityError(
        "stored candidate aggregate health is invalid");
  }
  return health;
}

struct EncodedSceneFinalized final {
  std::int64_t apply_generation;
  std::int64_t projection_stage_generation;
  std::int64_t candidate_scene_revision_id;
  std::string transition_evidence;
  std::string aggregate_health;
  std::string digest;
};

EncodedSceneFinalized
EncodeSceneFinalized(const ManagerApplySceneFinalizedDTO &finalized) {
  if (!finalized.valid()) {
    throw std::invalid_argument("scene Finalize evidence is invalid");
  }
  const auto &result = finalized.finalized_result;
  EncodedSceneFinalized encoded{
      CheckedSigned(result.apply_generation.value(), "apply_generation"),
      CheckedSigned(result.projection_stage_generation.value(),
                    "projection_stage_generation"),
      CheckedSigned(
          result.candidate_projection_revision.scene_revision.revision_id,
          "candidate_scene_revision_id"),
      EncodeTransitionEvidence(result.transition_evidence,
                               ProjectionTransitionEvidenceState::kFinalized),
      EncodeAggregateHealth(result.candidate_aggregate_health),
      {},
  };
  std::string record;
  AppendField(record, "domain", kSceneFinalizedEvidenceDigestDomain);
  AppendField(record, "publication_receipt_digest",
              finalized.publication_receipt_digest.value());
  AppendField(record, "apply_id", result.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(encoded.apply_generation));
  AppendField(record, "projection_stage_generation",
              std::to_string(encoded.projection_stage_generation));
  const auto &candidate = result.candidate_projection_revision;
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
  AppendField(record, "result_state", "finalized");
  AppendField(record, "transition_evidence_encoding",
              encoded.transition_evidence);
  AppendField(record, "candidate_aggregate_health_encoding",
              encoded.aggregate_health);
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
      row.Integer(14) !=
          static_cast<std::int64_t>(kMaximumManagerApplySceneStageIntents) ||
      row.Integer(15) !=
          static_cast<std::int64_t>(kMaximumManagerApplySceneStagedRecords) ||
      row.Integer(16) !=
          static_cast<std::int64_t>(kMaximumManagerApplyActivationSelections) ||
      row.Integer(17) != static_cast<std::int64_t>(
                             kMaximumManagerApplySceneActivatedRecords) ||
      row.Integer(18) != static_cast<std::int64_t>(
                             kMaximumManagerApplyPrivateMaterializations) ||
      row.Integer(19) !=
          static_cast<std::int64_t>(
              kMaximumManagerApplyRetiredCommandSurfaceRecords) ||
      row.Integer(20) !=
          static_cast<std::int64_t>(kMaximumManagerApplyPublicationReceipts) ||
      row.Integer(21) != static_cast<std::int64_t>(
                             kMaximumManagerApplySceneFinalizedRecords) ||
      row.Integer(22) !=
          static_cast<std::int64_t>(kMaximumManagerApplyFenceReleaseIntents) ||
      row.Integer(23) !=
          static_cast<std::int64_t>(kMaximumManagerApplyFenceReleasedRecords) ||
      row.Integer(24) !=
          static_cast<std::int64_t>(kMaximumManagerApplyTerminalOutcomes) ||
      row.Integer(25) !=
          static_cast<std::int64_t>(kMaximumManagerApplySceneTransitionRows) ||
      row.Integer(26) != kMaximumManagerApplyJournalDatabasePageImageBytes) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal resource-limit metadata mismatch");
  }
  if (row.Step()) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal metadata singleton is invalid");
  }
}

void ValidateApplyJournalIdentityAndSchema(sqlite3 *database,
                                           const CellId &cell_id) {
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
        "Apply-journal SQLite user version mismatch");
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
        [&cell_id](sqlite3 *database) {
          ValidateApplyJournalIdentityAndSchema(database, cell_id);
        }));
  } catch (const persistence::SqliteReadOnlyPreflightError &error) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal read-only preflight failed: " +
        std::string(error.what()));
  }
}

} // namespace

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
  return (direct || parent) && !request_context.request_id.empty() &&
         !request_context.request_owner_epoch.empty() &&
         !request_context.source_label.empty() &&
         (!request_context.correlation_id.has_value() ||
          !request_context.correlation_id->empty()) &&
         !apply_id.empty() && apply_generation.nonzero() && !cell_id.empty() &&
         expected_active_projection_revision.valid() &&
         !candidate_retained_bundle_id.empty() &&
         !candidate_graph_revision.empty();
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
      ValidateApplyJournalIdentityAndSchema(database_, cell_id_);
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

  ~Impl() {
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
          "maximum_scene_stage_intents,maximum_scene_staged_records,"
          "maximum_activation_selections,maximum_scene_activated_records,"
          "maximum_private_materializations,"
          "maximum_retired_command_surface_records,"
          "maximum_publication_receipts,maximum_scene_finalized_records,"
          "maximum_fence_release_intents,maximum_fence_released_records,"
          "maximum_terminal_outcomes,"
          "maximum_scene_transition_rows,"
          "maximum_main_database_page_image_bytes"
          ") VALUES (1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
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
      metadata.Bind(
          14, static_cast<std::int64_t>(kMaximumManagerApplySceneStageIntents));
      metadata.Bind(15, static_cast<std::int64_t>(
                            kMaximumManagerApplySceneStagedRecords));
      metadata.Bind(16, static_cast<std::int64_t>(
                            kMaximumManagerApplyActivationSelections));
      metadata.Bind(17, static_cast<std::int64_t>(
                            kMaximumManagerApplySceneActivatedRecords));
      metadata.Bind(18, static_cast<std::int64_t>(
                            kMaximumManagerApplyPrivateMaterializations));
      metadata.Bind(19, static_cast<std::int64_t>(
                            kMaximumManagerApplyRetiredCommandSurfaceRecords));
      metadata.Bind(20, static_cast<std::int64_t>(
                            kMaximumManagerApplyPublicationReceipts));
      metadata.Bind(21, static_cast<std::int64_t>(
                            kMaximumManagerApplySceneFinalizedRecords));
      metadata.Bind(22, static_cast<std::int64_t>(
                            kMaximumManagerApplyFenceReleaseIntents));
      metadata.Bind(23, static_cast<std::int64_t>(
                            kMaximumManagerApplyFenceReleasedRecords));
      metadata.Bind(
          24, static_cast<std::int64_t>(kMaximumManagerApplyTerminalOutcomes));
      metadata.Bind(25, static_cast<std::int64_t>(
                            kMaximumManagerApplySceneTransitionRows));
      metadata.Bind(26, kMaximumManagerApplyJournalDatabasePageImageBytes);
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
      VerifiedRecordMap<PersistedManagerApplySceneStageIntent>,
      VerifiedRecordMap<PersistedManagerApplySceneStaged>,
      VerifiedRecordMap<PersistedManagerApplyActivationSelection>,
      VerifiedRecordMap<PersistedManagerApplySceneActivated>,
      VerifiedRecordMap<PersistedManagerApplyPrivateMaterialization>,
      VerifiedRecordMap<PersistedManagerApplyObsoleteCommandSurfacesRetired>,
      VerifiedRecordMap<PersistedManagerApplyPublicationReceipt>,
      VerifiedRecordMap<PersistedManagerApplySceneFinalized>,
      VerifiedRecordMap<PersistedManagerApplyFenceReleaseIntent>,
      VerifiedRecordMap<PersistedManagerApplyFenceReleased>,
      VerifiedRecordMap<PersistedManagerApplyTerminalOutcome>>;

  // Reuse verified predecessors only within one locked SQLite read. No
  // result (including absence) survives the outer read or an exception.
  // Subsequent operations still read and validate the database afresh.
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
    records.emplace(key, result);
    return result;
  }

  std::optional<PersistedManagerApplyIntent>
  IntentLocked(const ApplyId &apply_id,
               const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyIntent>(
        {apply_id, apply_generation, 0},
        [&] { return IntentUncachedLocked(apply_id, apply_generation); });
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
        "expected_admission_generation,parent_authoring_claim,intent_digest "
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
    std::optional<core::AdmissionGeneration> expected_admission_generation;
    if (!row.IsNull(13)) {
      const auto stored_admission_generation = row.Integer(13);
      if (stored_admission_generation <= 0) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply admission generation is invalid");
      }
      expected_admission_generation.emplace(
          static_cast<std::uint64_t>(stored_admission_generation));
    }
    const auto parent_authoring_claim = row.OptionalBlob(14);
    const auto stored_digest = row.Text(15);
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
        expected_admission_generation,
        parent_authoring_claim.has_value()
            ? std::optional<core::CellAdmissionClaimDTO>(
                  DecodeParentAuthoringClaim(*parent_authoring_claim))
            : std::nullopt,
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
                  "affected_scope_digest FROM manager_apply_affected_scopes "
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
          DecodeActiveConfigurations(active_configurations),
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
          return RuntimeStagedReadyUncachedLocked(apply_id, apply_generation,
                                                  staged_ready_generation);
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
                                 const std::int64_t apply_generation) const {
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

  std::optional<PersistedManagerApplySceneStageIntent>
  SceneStageIntentLocked(const ApplyId &apply_id,
                         const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplySceneStageIntent>(
        {apply_id, apply_generation, 0}, [&] {
          return SceneStageIntentUncachedLocked(apply_id, apply_generation);
        });
  }

  std::optional<PersistedManagerApplySceneStageIntent>
  SceneStageIntentUncachedLocked(const ApplyId &apply_id,
                                 const std::int64_t apply_generation) const {
    Statement row(database_,
                  "SELECT runtime_staged_ready_digest,"
                  "predecessor_publication_generation,"
                  "predecessor_public_pointer_digest,"
                  "predecessor_retained_bundle_id,predecessor_graph_revision,"
                  "predecessor_scene_id,predecessor_scene_revision_id,"
                  "predecessor_scene_content_hash,candidate_retained_bundle_id,"
                  "candidate_graph_revision,native_transition_rows_encoding,"
                  "scene_stage_intent_digest "
                  "FROM manager_apply_scene_stage_intents "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto staged_ready_digest = row.Text(0);
    const auto publication_generation = row.Integer(1);
    const auto public_pointer_digest = row.Text(2);
    const auto predecessor_bundle = row.Text(3);
    const auto predecessor_graph = row.Text(4);
    const auto predecessor_scene_id = row.Text(5);
    const auto predecessor_scene_revision = row.Integer(6);
    const auto predecessor_scene_digest = row.Text(7);
    const auto candidate_bundle = row.Text(8);
    const auto candidate_graph = row.Text(9);
    const auto transition_rows = row.Text(10);
    const auto stored_digest = row.Text(11);
    if (row.Step() || apply_generation <= 0 || publication_generation <= 0 ||
        predecessor_scene_revision <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored scene-stage intent is structurally invalid");
    }
    try {
      ManagerApplySceneStageIntentDTO intent{
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          ContentDigest(staged_ready_digest),
          core::ActiveBundlePublicationGeneration(
              static_cast<std::uint64_t>(publication_generation)),
          ContentDigest(public_pointer_digest),
          ProjectionRevisionDTO{
              RetainedBundleId(predecessor_bundle),
              GraphRevision(predecessor_graph),
              SceneRevision{
                  SceneId(predecessor_scene_id),
                  static_cast<std::uint64_t>(predecessor_scene_revision),
                  ContentDigest(predecessor_scene_digest),
              },
          },
          RetainedBundleId(candidate_bundle),
          GraphRevision(candidate_graph),
          DecodeTransitionRows(transition_rows),
      };
      const auto encoded = EncodeSceneStageIntent(intent);
      const auto staged = RuntimeStagedReadyByDigestLocked(
          apply_id, apply_generation, intent.runtime_staged_ready_digest);
      const auto parent = IntentLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !staged.has_value() ||
          !WithdrawalsMatchRuntimeStage(intent, staged->staged_ready) ||
          staged->staged_ready.predecessor_publication_generation !=
              intent.predecessor_publication_generation ||
          staged->staged_ready.predecessor_public_pointer_digest !=
              intent.predecessor_public_pointer_digest ||
          !parent.has_value() ||
          parent->intent.expected_active_projection_revision !=
              intent.expected_active_projection_revision ||
          parent->intent.candidate_retained_bundle_id !=
              intent.candidate_retained_bundle_id ||
          parent->intent.candidate_graph_revision !=
              intent.candidate_graph_revision) {
        throw ManagerApplyJournalIntegrityError(
            "stored scene-stage intent is not bound to its exact "
            "staged-ready/Apply records");
      }
      return PersistedManagerApplySceneStageIntent{
          std::move(intent), ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored scene-stage intent contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplySceneStaged>
  SceneStagedLocked(const ApplyId &apply_id,
                    const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplySceneStaged>(
        {apply_id, apply_generation, 0},
        [&] { return SceneStagedUncachedLocked(apply_id, apply_generation); });
  }

  std::optional<PersistedManagerApplySceneStaged>
  SceneStagedUncachedLocked(const ApplyId &apply_id,
                            const std::int64_t apply_generation) const {
    Statement row(database_,
                  "SELECT scene_stage_intent_digest,"
                  "predecessor_retained_bundle_id,predecessor_graph_revision,"
                  "predecessor_scene_id,predecessor_scene_revision_id,"
                  "predecessor_scene_content_hash,projection_stage_generation,"
                  "candidate_retained_bundle_id,candidate_graph_revision,"
                  "candidate_scene_id,candidate_scene_revision_id,"
                  "candidate_scene_content_hash,result_state,"
                  "transition_evidence_encoding,scene_staged_digest "
                  "FROM manager_apply_scene_staged_records "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto stage_intent_digest = row.Text(0);
    const auto predecessor_bundle = row.Text(1);
    const auto predecessor_graph = row.Text(2);
    const auto predecessor_scene_id = row.Text(3);
    const auto predecessor_scene_revision = row.Integer(4);
    const auto predecessor_scene_digest = row.Text(5);
    const auto stage_generation = row.Integer(6);
    const auto candidate_bundle = row.Text(7);
    const auto candidate_graph = row.Text(8);
    const auto candidate_scene_id = row.Text(9);
    const auto candidate_scene_revision = row.Integer(10);
    const auto candidate_scene_digest = row.Text(11);
    const auto result_state = row.Text(12);
    const auto transition_evidence = row.Text(13);
    const auto stored_digest = row.Text(14);
    if (row.Step() || apply_generation <= 0 ||
        predecessor_scene_revision <= 0 || stage_generation <= 0 ||
        candidate_scene_revision <= 0 || result_state != "staged") {
      throw ManagerApplyJournalIntegrityError(
          "stored scene staged evidence is structurally invalid");
    }
    try {
      const ProjectionRevisionDTO predecessor{
          RetainedBundleId(predecessor_bundle),
          GraphRevision(predecessor_graph),
          SceneRevision{
              SceneId(predecessor_scene_id),
              static_cast<std::uint64_t>(predecessor_scene_revision),
              ContentDigest(predecessor_scene_digest),
          },
      };
      const ProjectionRevisionDTO candidate{
          RetainedBundleId(candidate_bundle),
          GraphRevision(candidate_graph),
          SceneRevision{
              SceneId(candidate_scene_id),
              static_cast<std::uint64_t>(candidate_scene_revision),
              ContentDigest(candidate_scene_digest),
          },
      };
      ManagerApplySceneStagedDTO staged{
          ContentDigest(stage_intent_digest),
          ProjectionApplyStageResultDTO{
              apply_id,
              ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
              predecessor,
              candidate,
              ProjectionStageGeneration(
                  static_cast<std::uint64_t>(stage_generation)),
              DecodeTransitionEvidence(
                  transition_evidence,
                  ProjectionTransitionEvidenceState::kStaged),
              ProjectionApplyStageResultState::kStaged,
              std::nullopt,
          },
      };
      const auto encoded = EncodeSceneStaged(staged);
      const auto intent = SceneStageIntentLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !intent.has_value() ||
          intent->scene_stage_intent_digest !=
              staged.scene_stage_intent_digest ||
          staged.staged_result.expected_active_projection_revision !=
              intent->scene_stage_intent.expected_active_projection_revision ||
          candidate.retained_bundle_id !=
              intent->scene_stage_intent.candidate_retained_bundle_id ||
          candidate.graph_revision !=
              intent->scene_stage_intent.candidate_graph_revision ||
          staged.staged_result.transition_evidence->transition_rows.size() !=
              intent->scene_stage_intent.native_transition_rows.size()) {
        throw ManagerApplyJournalIntegrityError(
            "stored scene staged evidence is not bound to its exact "
            "scene-stage intent");
      }
      for (std::size_t index = 0U;
           index < intent->scene_stage_intent.native_transition_rows.size();
           ++index) {
        if (staged.staged_result.transition_evidence->transition_rows[index]
                .transition_row !=
            intent->scene_stage_intent.native_transition_rows[index]) {
          throw ManagerApplyJournalIntegrityError(
              "stored scene staged transition set differs from its intent");
        }
      }
      return PersistedManagerApplySceneStaged{std::move(staged),
                                              ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored scene staged evidence contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplyActivationSelection>
  ActivationSelectionLocked(const ApplyId &apply_id,
                            const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplyActivationSelection>(
        {apply_id, apply_generation, 0}, [&] {
          return ActivationSelectionUncachedLocked(apply_id, apply_generation);
        });
  }

  std::optional<PersistedManagerApplyActivationSelection>
  ActivationSelectionUncachedLocked(const ApplyId &apply_id,
                                    const std::int64_t apply_generation) const {
    Statement row(
        database_,
        "SELECT scene_staged_digest,predecessor_retained_bundle_id,"
        "predecessor_graph_revision,predecessor_scene_id,"
        "predecessor_scene_revision_id,predecessor_scene_content_hash,"
        "projection_stage_generation,candidate_retained_bundle_id,"
        "candidate_graph_revision,candidate_scene_id,"
        "candidate_scene_revision_id,candidate_scene_content_hash,"
        "activation_selection_digest "
        "FROM manager_apply_activation_selections "
        "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto scene_staged_digest = row.Text(0);
    const auto predecessor_bundle = row.Text(1);
    const auto predecessor_graph = row.Text(2);
    const auto predecessor_scene_id = row.Text(3);
    const auto predecessor_scene_revision = row.Integer(4);
    const auto predecessor_scene_digest = row.Text(5);
    const auto stage_generation = row.Integer(6);
    const auto candidate_bundle = row.Text(7);
    const auto candidate_graph = row.Text(8);
    const auto candidate_scene_id = row.Text(9);
    const auto candidate_scene_revision = row.Integer(10);
    const auto candidate_scene_digest = row.Text(11);
    const auto stored_digest = row.Text(12);
    if (row.Step() || apply_generation <= 0 ||
        predecessor_scene_revision <= 0 || stage_generation <= 0 ||
        candidate_scene_revision <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored activation selection is structurally invalid");
    }
    try {
      ManagerApplyActivationSelectionDTO selection{
          ContentDigest(scene_staged_digest),
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          ProjectionRevisionDTO{
              RetainedBundleId(predecessor_bundle),
              GraphRevision(predecessor_graph),
              SceneRevision{
                  SceneId(predecessor_scene_id),
                  static_cast<std::uint64_t>(predecessor_scene_revision),
                  ContentDigest(predecessor_scene_digest),
              },
          },
          ProjectionStageGeneration(
              static_cast<std::uint64_t>(stage_generation)),
          ProjectionRevisionDTO{
              RetainedBundleId(candidate_bundle),
              GraphRevision(candidate_graph),
              SceneRevision{
                  SceneId(candidate_scene_id),
                  static_cast<std::uint64_t>(candidate_scene_revision),
                  ContentDigest(candidate_scene_digest),
              },
          },
      };
      const auto encoded = EncodeActivationSelection(selection);
      const auto staged = SceneStagedLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !staged.has_value() ||
          staged->scene_staged_digest != selection.scene_staged_digest ||
          staged->scene_staged.staged_result
                  .expected_active_projection_revision !=
              selection.predecessor_projection_revision ||
          *staged->scene_staged.staged_result.projection_stage_generation !=
              selection.projection_stage_generation ||
          *staged->scene_staged.staged_result.candidate_projection_revision !=
              selection.candidate_projection_revision) {
        throw ManagerApplyJournalIntegrityError(
            "stored activation selection is not bound to exact staged "
            "evidence");
      }
      return PersistedManagerApplyActivationSelection{
          std::move(selection), ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored activation selection contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplySceneActivated>
  SceneActivatedLocked(const ApplyId &apply_id,
                       const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplySceneActivated>(
        {apply_id, apply_generation, 0}, [&] {
          return SceneActivatedUncachedLocked(apply_id, apply_generation);
        });
  }

  std::optional<PersistedManagerApplySceneActivated>
  SceneActivatedUncachedLocked(const ApplyId &apply_id,
                               const std::int64_t apply_generation) const {
    Statement row(database_,
                  "SELECT activation_selection_digest,"
                  "predecessor_retained_bundle_id,predecessor_graph_revision,"
                  "predecessor_scene_id,predecessor_scene_revision_id,"
                  "predecessor_scene_content_hash,projection_stage_generation,"
                  "candidate_retained_bundle_id,candidate_graph_revision,"
                  "candidate_scene_id,candidate_scene_revision_id,"
                  "candidate_scene_content_hash,result_state,"
                  "transition_evidence_encoding,scene_activated_digest "
                  "FROM manager_apply_scene_activated_records "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto selection_digest = row.Text(0);
    const auto predecessor_bundle = row.Text(1);
    const auto predecessor_graph = row.Text(2);
    const auto predecessor_scene_id = row.Text(3);
    const auto predecessor_scene_revision = row.Integer(4);
    const auto predecessor_scene_digest = row.Text(5);
    const auto stage_generation = row.Integer(6);
    const auto candidate_bundle = row.Text(7);
    const auto candidate_graph = row.Text(8);
    const auto candidate_scene_id = row.Text(9);
    const auto candidate_scene_revision = row.Integer(10);
    const auto candidate_scene_digest = row.Text(11);
    const auto result_state = row.Text(12);
    const auto transition_evidence = row.Text(13);
    const auto stored_digest = row.Text(14);
    if (row.Step() || apply_generation <= 0 ||
        predecessor_scene_revision <= 0 || stage_generation <= 0 ||
        candidate_scene_revision <= 0 || result_state != "activated") {
      throw ManagerApplyJournalIntegrityError(
          "stored scene activation evidence is structurally invalid");
    }
    try {
      const ProjectionRevisionDTO predecessor{
          RetainedBundleId(predecessor_bundle),
          GraphRevision(predecessor_graph),
          SceneRevision{
              SceneId(predecessor_scene_id),
              static_cast<std::uint64_t>(predecessor_scene_revision),
              ContentDigest(predecessor_scene_digest),
          },
      };
      const ProjectionRevisionDTO candidate{
          RetainedBundleId(candidate_bundle),
          GraphRevision(candidate_graph),
          SceneRevision{
              SceneId(candidate_scene_id),
              static_cast<std::uint64_t>(candidate_scene_revision),
              ContentDigest(candidate_scene_digest),
          },
      };
      ManagerApplySceneActivatedDTO activated{
          ContentDigest(selection_digest),
          ProjectionApplyActivationResultDTO{
              apply_id,
              ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
              predecessor,
              ProjectionStageGeneration(
                  static_cast<std::uint64_t>(stage_generation)),
              candidate,
              ProjectionApplyActivationResultState::kActivated,
              candidate,
              DecodeTransitionEvidence(
                  transition_evidence,
                  ProjectionTransitionEvidenceState::kActivatedAndRetired),
              std::nullopt,
          },
      };
      const auto encoded = EncodeSceneActivated(activated);
      const auto selection =
          ActivationSelectionLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !selection.has_value() ||
          selection->activation_selection_digest !=
              activated.activation_selection_digest ||
          selection->activation_selection.predecessor_projection_revision !=
              predecessor ||
          selection->activation_selection.projection_stage_generation !=
              activated.activation_result.projection_stage_generation ||
          selection->activation_selection.candidate_projection_revision !=
              candidate) {
        throw ManagerApplyJournalIntegrityError(
            "stored scene activation is not bound to exact selection");
      }
      return PersistedManagerApplySceneActivated{std::move(activated),
                                                 ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &error) {
      throw ManagerApplyJournalIntegrityError(
          "stored scene activation evidence contains an invalid value: " +
          std::string(error.what()));
    }
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
                  "SELECT scene_activated_digest,projection_stage_generation,"
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
    const auto activated_digest = row.Text(0);
    const auto stage_generation = row.Integer(1);
    const auto candidate_bundle = row.Text(2);
    const auto candidate_graph = row.Text(3);
    const auto candidate_scene_id = row.Text(4);
    const auto candidate_scene_revision = row.Integer(5);
    const auto candidate_scene_digest = row.Text(6);
    const auto canonical_digest = row.Text(7);
    const auto canonical_size = row.Integer(8);
    const auto stored_digest = row.Text(9);
    if (row.Step() || apply_generation <= 0 || stage_generation <= 0 ||
        candidate_scene_revision <= 0 || canonical_size <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored private materialization is structurally invalid");
    }
    try {
      ManagerApplyPrivateMaterializationDTO materialization{
          ContentDigest(activated_digest),
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          ProjectionStageGeneration(
              static_cast<std::uint64_t>(stage_generation)),
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
      const auto activated = SceneActivatedLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !activated.has_value() ||
          activated->scene_activated_digest !=
              materialization.scene_activated_digest ||
          activated->scene_activated.activation_result
                  .projection_stage_generation !=
              materialization.projection_stage_generation ||
          activated->scene_activated.activation_result
                  .candidate_projection_revision !=
              materialization.candidate_projection_revision) {
        throw ManagerApplyJournalIntegrityError(
            "stored private materialization is not bound to exact activated "
            "evidence");
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

  std::optional<PersistedManagerApplyObsoleteCommandSurfacesRetired>
  RetiredCommandSurfacesLocked(const ApplyId &apply_id,
                               const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<
        PersistedManagerApplyObsoleteCommandSurfacesRetired>(
        {apply_id, apply_generation, 0}, [&] {
          return RetiredCommandSurfacesUncachedLocked(apply_id,
                                                      apply_generation);
        });
  }

  std::optional<PersistedManagerApplyObsoleteCommandSurfacesRetired>
  RetiredCommandSurfacesUncachedLocked(
      const ApplyId &apply_id, const std::int64_t apply_generation) const {
    Statement row(
        database_,
        "SELECT private_materialization_content_digest,"
        "command_surfaces_encoding,retired_command_surfaces_evidence_digest "
        "FROM manager_apply_retired_command_surfaces "
        "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto materialization_digest = row.Text(0);
    const auto command_surfaces = row.Text(1);
    const auto stored_digest = row.Text(2);
    if (row.Step() || apply_generation <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored retired command surfaces are structurally invalid");
    }
    try {
      ManagerApplyObsoleteCommandSurfacesRetiredDTO retired{
          ContentDigest(materialization_digest),
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          DecodeObsoleteCommandSurfaces(command_surfaces),
      };
      const auto encoded = EncodeRetiredCommandSurfaces(retired);
      const auto materialization =
          PrivateMaterializationLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !materialization.has_value() ||
          materialization->private_materialization_content_digest !=
              retired.private_materialization_content_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored retired command surfaces are not bound to the exact "
            "private materialization");
      }
      return PersistedManagerApplyObsoleteCommandSurfacesRetired{
          std::move(retired), ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored retired command surfaces contain an invalid value");
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
        "SELECT retired_command_surfaces_evidence_digest,"
        "projection_stage_generation,candidate_retained_bundle_id,"
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
    const auto retired_digest = row.Text(0);
    const auto stage_generation = row.Integer(1);
    const auto candidate_bundle = row.Text(2);
    const auto candidate_graph = row.Text(3);
    const auto candidate_scene_id = row.Text(4);
    const auto candidate_scene_revision = row.Integer(5);
    const auto candidate_scene_digest = row.Text(6);
    const auto previous_generation = row.Integer(7);
    const auto materialization_digest = row.Text(8);
    const auto publication_generation = row.Integer(9);
    const auto public_pointer_digest = row.Text(10);
    const auto stored_digest = row.Text(11);
    if (row.Step() || apply_generation <= 0 || stage_generation <= 0 ||
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
          ContentDigest(retired_digest),
          ActiveBundlePublicationReceiptDTO{
              ActiveBundlePublicationRequestDTO{
                  NamedApplyPublicationTransactionDTO{
                      apply_id,
                      ApplyGeneration(
                          static_cast<std::uint64_t>(apply_generation)),
                      ProjectionStageGeneration(
                          static_cast<std::uint64_t>(stage_generation)),
                  },
                  candidate,
                  candidate.retained_bundle_id,
                  candidate.graph_revision,
                  ActiveBundlePublicationGeneration(
                      static_cast<std::uint64_t>(previous_generation)),
                  ContentDigest(materialization_digest),
                  ContentDigest(retired_digest),
              },
              ActiveBundlePublicationGeneration(
                  static_cast<std::uint64_t>(publication_generation)),
              ContentDigest(public_pointer_digest),
          },
      };
      const auto encoded = EncodePublicationReceipt(publication);
      const auto retired =
          RetiredCommandSurfacesLocked(apply_id, apply_generation);
      const auto selection =
          ActivationSelectionLocked(apply_id, apply_generation);
      const auto stage_intent =
          SceneStageIntentLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !retired.has_value() ||
          retired->obsolete_managed_command_surfaces_retired_evidence_digest !=
              publication
                  .obsolete_managed_command_surfaces_retired_evidence_digest ||
          retired->retired_command_surfaces
                  .private_materialization_content_digest !=
              publication.publication_receipt.request
                  .private_materialization_content_digest ||
          !selection.has_value() ||
          selection->activation_selection.projection_stage_generation !=
              ProjectionStageGeneration(
                  static_cast<std::uint64_t>(stage_generation)) ||
          selection->activation_selection.candidate_projection_revision !=
              candidate ||
          !stage_intent.has_value() ||
          stage_intent->scene_stage_intent.predecessor_publication_generation !=
              ActiveBundlePublicationGeneration(
                  static_cast<std::uint64_t>(previous_generation))) {
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

  std::optional<PersistedManagerApplySceneFinalized>
  SceneFinalizedLocked(const ApplyId &apply_id,
                       const std::int64_t apply_generation) const {
    return ReadVerifiedRecord<PersistedManagerApplySceneFinalized>(
        {apply_id, apply_generation, 0}, [&] {
          return SceneFinalizedUncachedLocked(apply_id, apply_generation);
        });
  }

  std::optional<PersistedManagerApplySceneFinalized>
  SceneFinalizedUncachedLocked(const ApplyId &apply_id,
                               const std::int64_t apply_generation) const {
    Statement row(
        database_,
        "SELECT publication_receipt_digest,projection_stage_generation,"
        "candidate_retained_bundle_id,candidate_graph_revision,"
        "candidate_scene_id,candidate_scene_revision_id,"
        "candidate_scene_content_hash,transition_evidence_encoding,"
        "candidate_aggregate_health_encoding,scene_finalized_digest "
        "FROM manager_apply_scene_finalized_records "
        "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto publication_digest = row.Text(0);
    const auto stage_generation = row.Integer(1);
    const auto candidate_bundle = row.Text(2);
    const auto candidate_graph = row.Text(3);
    const auto candidate_scene_id = row.Text(4);
    const auto candidate_scene_revision = row.Integer(5);
    const auto candidate_scene_digest = row.Text(6);
    const auto transition_evidence = row.Text(7);
    const auto aggregate_health = row.Text(8);
    const auto stored_digest = row.Text(9);
    if (row.Step() || apply_generation <= 0 || stage_generation <= 0 ||
        candidate_scene_revision <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored scene Finalize evidence is structurally invalid");
    }
    try {
      const auto publication =
          PublicationReceiptLocked(apply_id, apply_generation);
      if (!publication.has_value() || publication->publication_receipt_digest !=
                                          ContentDigest(publication_digest)) {
        throw ManagerApplyJournalIntegrityError(
            "stored scene Finalize evidence lacks its exact publication");
      }
      const ProjectionRevisionDTO candidate{
          RetainedBundleId(candidate_bundle),
          GraphRevision(candidate_graph),
          SceneRevision{
              SceneId(candidate_scene_id),
              static_cast<std::uint64_t>(candidate_scene_revision),
              ContentDigest(candidate_scene_digest),
          },
      };
      ManagerApplySceneFinalizedDTO finalized{
          ContentDigest(publication_digest),
          ProjectionApplyFinalizeResultDTO{
              apply_id,
              ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
              ProjectionStageGeneration(
                  static_cast<std::uint64_t>(stage_generation)),
              candidate,
              publication->publication.publication_receipt,
              ProjectionApplyFinalizeResultState::kFinalized,
              candidate,
              DecodeTransitionEvidence(
                  transition_evidence,
                  ProjectionTransitionEvidenceState::kFinalized),
              DecodeAggregateHealth(aggregate_health),
              std::nullopt,
          },
      };
      const auto encoded = EncodeSceneFinalized(finalized);
      const auto activated = SceneActivatedLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !activated.has_value() ||
          activated->scene_activated.activation_result
                  .projection_stage_generation !=
              finalized.finalized_result.projection_stage_generation ||
          activated->scene_activated.activation_result
                  .candidate_projection_revision != candidate ||
          activated->scene_activated.activation_result.transition_evidence
                  .transition_rows.size() !=
              finalized.finalized_result.transition_evidence.transition_rows
                  .size()) {
        throw ManagerApplyJournalIntegrityError(
            "stored scene Finalize evidence is not bound to exact activation");
      }
      for (std::size_t index = 0U;
           index < finalized.finalized_result.transition_evidence
                       .transition_rows.size();
           ++index) {
        if (activated->scene_activated.activation_result.transition_evidence
                .transition_rows[index]
                .transition_row !=
            finalized.finalized_result.transition_evidence
                .transition_rows[index]
                .transition_row) {
          throw ManagerApplyJournalIntegrityError(
              "stored finalized transition set differs from activation");
        }
      }
      return PersistedManagerApplySceneFinalized{std::move(finalized),
                                                 ContentDigest(stored_digest)};
    } catch (const ManagerApplyJournalIntegrityError &) {
      throw;
    } catch (const std::exception &) {
      throw ManagerApplyJournalIntegrityError(
          "stored scene Finalize evidence contains an invalid value");
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
                  "SELECT scene_finalized_digest,fence_acceptance_digest,"
                  "release_request_encoding,fence_release_intent_digest "
                  "FROM manager_apply_fence_release_intents "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto scene_finalized_digest = row.Text(0);
    const auto fence_acceptance_digest = row.Text(1);
    const auto request_encoding = row.Text(2);
    const auto stored_digest = row.Text(3);
    if (row.Step() || apply_generation <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "stored fence-release intent is structurally invalid");
    }
    try {
      ManagerApplyFenceReleaseIntentDTO intent{
          ContentDigest(scene_finalized_digest),
          ContentDigest(fence_acceptance_digest),
          DecodeFenceReleaseRequest(request_encoding),
      };
      const auto encoded = EncodeFenceReleaseIntent(intent);
      const auto &evidence = intent.release_request.reconciliation_evidence;
      const auto finalized = SceneFinalizedLocked(apply_id, apply_generation);
      const auto acceptance = FenceAcceptanceLocked(apply_id, apply_generation);
      const auto publication =
          PublicationReceiptLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest ||
          evidence.owner_transaction.apply_id != apply_id ||
          evidence.owner_transaction.apply_generation.value() !=
              static_cast<std::uint64_t>(apply_generation) ||
          !finalized.has_value() ||
          finalized->scene_finalized_digest != intent.scene_finalized_digest ||
          !acceptance.has_value() ||
          acceptance->acceptance_digest != intent.fence_acceptance_digest ||
          acceptance->accepted_fence.configuration_fence_id !=
              evidence.configuration_fence_id ||
          acceptance->accepted_fence.configuration_fence_generation !=
              evidence.configuration_fence_generation ||
          acceptance->accepted_fence.captured_scope !=
              evidence.reconciled_scope ||
          finalized->scene_finalized.finalized_result
                  .candidate_projection_revision !=
              evidence.active_projection_revision ||
          !publication.has_value() ||
          publication->publication.publication_receipt.publication_generation !=
              evidence.active_publication_generation ||
          publication->publication.publication_receipt
                  .durable_public_pointer_content_digest !=
              evidence.active_public_pointer_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored fence-release intent is not bound to exact finalized, "
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
                  "SELECT fence_released_digest,scene_finalized_digest,"
                  "publication_receipt_digest,candidate_retained_bundle_id,"
                  "candidate_graph_revision,candidate_scene_id,"
                  "candidate_scene_revision_id,candidate_scene_content_hash,"
                  "outcome_state,terminal_outcome_digest "
                  "FROM manager_apply_terminal_outcomes "
                  "WHERE apply_id=? AND apply_generation=?");
    row.Bind(1, apply_id.value());
    row.Bind(2, apply_generation);
    if (!row.Step()) {
      return std::nullopt;
    }
    const auto fence_released_digest = row.Text(0);
    const auto scene_finalized_digest = row.Text(1);
    const auto publication_digest = row.Text(2);
    const auto candidate_bundle = row.Text(3);
    const auto candidate_graph = row.Text(4);
    const auto candidate_scene = row.Text(5);
    const auto candidate_scene_revision = row.Integer(6);
    const auto candidate_scene_digest = row.Text(7);
    const auto state = row.Text(8);
    const auto stored_digest = row.Text(9);
    if (row.Step() || apply_generation <= 0 || candidate_scene_revision <= 0 ||
        state != "succeeded") {
      throw ManagerApplyJournalIntegrityError(
          "stored terminal Apply outcome is structurally invalid");
    }
    try {
      ManagerApplyTerminalOutcomeDTO outcome{
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          ProjectionRevisionDTO{
              RetainedBundleId(candidate_bundle),
              GraphRevision(candidate_graph),
              SceneRevision{
                  SceneId(candidate_scene),
                  static_cast<std::uint64_t>(candidate_scene_revision),
                  ContentDigest(candidate_scene_digest),
              },
          },
          ContentDigest(publication_digest),
          ContentDigest(scene_finalized_digest),
          ContentDigest(fence_released_digest),
          ManagerApplyTerminalOutcomeState::kSucceeded,
      };
      const auto encoded = EncodeTerminalOutcome(outcome);
      const auto released = FenceReleasedLocked(apply_id, apply_generation);
      const auto finalized = SceneFinalizedLocked(apply_id, apply_generation);
      const auto publication =
          PublicationReceiptLocked(apply_id, apply_generation);
      if (encoded.digest != stored_digest || !released.has_value() ||
          released->fence_released_digest != outcome.fence_released_digest ||
          !finalized.has_value() ||
          finalized->scene_finalized_digest != outcome.scene_finalized_digest ||
          finalized->scene_finalized.finalized_result
                  .candidate_projection_revision !=
              outcome.candidate_projection_revision ||
          !publication.has_value() ||
          publication->publication_receipt_digest !=
              outcome.publication_receipt_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored terminal Apply outcome is not bound to exact completion "
            "evidence");
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

  ManagerApplyJournalIntegrityReport VerifyIntegrityLocked() const {
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
    const auto scene_stage_intent_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_scene_stage_intents");
    const auto scene_staged_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_scene_staged_records");
    const auto activation_selection_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_activation_selections");
    const auto scene_activated_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_apply_scene_activated_records");
    const auto private_materialization_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_apply_private_materializations");
    const auto retired_command_surfaces_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_apply_retired_command_surfaces");
    const auto publication_receipt_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_publication_receipts");
    const auto scene_finalized_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_apply_scene_finalized_records");
    const auto fence_release_intent_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_fence_release_intents");
    const auto fence_released_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_fence_released_records");
    const auto terminal_outcome_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_terminal_outcomes");
    if (affected_scope_count < 0 || attempt_count < 0 || acceptance_count < 0 ||
        runtime_intent_count < 0 || runtime_staged_ready_count < 0 ||
        scene_stage_intent_count < 0 || scene_staged_count < 0 ||
        activation_selection_count < 0 || scene_activated_count < 0 ||
        private_materialization_count < 0 ||
        retired_command_surfaces_count < 0 || publication_receipt_count < 0 ||
        scene_finalized_count < 0 || fence_release_intent_count < 0 ||
        fence_released_count < 0 || terminal_outcome_count < 0 ||
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
        scene_stage_intent_count >
            static_cast<std::int64_t>(kMaximumManagerApplySceneStageIntents) ||
        scene_staged_count >
            static_cast<std::int64_t>(kMaximumManagerApplySceneStagedRecords) ||
        activation_selection_count >
            static_cast<std::int64_t>(
                kMaximumManagerApplyActivationSelections) ||
        scene_activated_count >
            static_cast<std::int64_t>(
                kMaximumManagerApplySceneActivatedRecords) ||
        private_materialization_count >
            static_cast<std::int64_t>(
                kMaximumManagerApplyPrivateMaterializations) ||
        retired_command_surfaces_count >
            static_cast<std::int64_t>(
                kMaximumManagerApplyRetiredCommandSurfaceRecords) ||
        publication_receipt_count >
            static_cast<std::int64_t>(
                kMaximumManagerApplyPublicationReceipts) ||
        scene_finalized_count >
            static_cast<std::int64_t>(
                kMaximumManagerApplySceneFinalizedRecords) ||
        fence_release_intent_count >
            static_cast<std::int64_t>(
                kMaximumManagerApplyFenceReleaseIntents) ||
        fence_released_count > static_cast<std::int64_t>(
                                   kMaximumManagerApplyFenceReleasedRecords) ||
        terminal_outcome_count >
            static_cast<std::int64_t>(kMaximumManagerApplyTerminalOutcomes) ||
        terminal_outcome_count > fence_released_count ||
        fence_released_count > fence_release_intent_count ||
        fence_release_intent_count > scene_finalized_count ||
        scene_finalized_count > publication_receipt_count ||
        publication_receipt_count > retired_command_surfaces_count ||
        retired_command_surfaces_count > private_materialization_count ||
        private_materialization_count > scene_activated_count ||
        scene_activated_count > activation_selection_count ||
        activation_selection_count > scene_staged_count ||
        scene_staged_count > scene_stage_intent_count ||
        scene_stage_intent_count > runtime_intent_count ||
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
    Statement scene_stage_intents(
        database_,
        "SELECT apply_id,apply_generation "
        "FROM manager_apply_scene_stage_intents "
        "ORDER BY apply_id,apply_generation LIMIT ?");
    scene_stage_intents.Bind(
        1,
        static_cast<std::int64_t>(kMaximumManagerApplySceneStageIntents + 1U));
    std::int64_t observed_scene_stage_intents = 0;
    while (scene_stage_intents.Step()) {
      ++observed_scene_stage_intents;
      if (observed_scene_stage_intents >
              static_cast<std::int64_t>(
                  kMaximumManagerApplySceneStageIntents) ||
          !SceneStageIntentLocked(ApplyId(scene_stage_intents.Text(0)),
                                  scene_stage_intents.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored scene-stage intent inventory is inconsistent");
      }
    }
    Statement scene_staged_records(
        database_, "SELECT apply_id,apply_generation "
                   "FROM manager_apply_scene_staged_records "
                   "ORDER BY apply_id,apply_generation LIMIT ?");
    scene_staged_records.Bind(
        1,
        static_cast<std::int64_t>(kMaximumManagerApplySceneStagedRecords + 1U));
    std::int64_t observed_scene_staged = 0;
    while (scene_staged_records.Step()) {
      ++observed_scene_staged;
      if (observed_scene_staged > static_cast<std::int64_t>(
                                      kMaximumManagerApplySceneStagedRecords) ||
          !SceneStagedLocked(ApplyId(scene_staged_records.Text(0)),
                             scene_staged_records.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored scene staged-evidence inventory is inconsistent");
      }
    }
    if (observed_scene_stage_intents != scene_stage_intent_count ||
        observed_scene_staged != scene_staged_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored scene-stage inventory count is inconsistent");
    }
    Statement activation_selections(
        database_, "SELECT apply_id,apply_generation "
                   "FROM manager_apply_activation_selections "
                   "ORDER BY apply_id,apply_generation LIMIT ?");
    activation_selections.Bind(
        1, static_cast<std::int64_t>(kMaximumManagerApplyActivationSelections +
                                     1U));
    std::int64_t observed_activation_selections = 0;
    while (activation_selections.Step()) {
      ++observed_activation_selections;
      if (observed_activation_selections >
              static_cast<std::int64_t>(
                  kMaximumManagerApplyActivationSelections) ||
          !ActivationSelectionLocked(ApplyId(activation_selections.Text(0)),
                                     activation_selections.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored activation-selection inventory is inconsistent");
      }
    }
    Statement scene_activated_records(
        database_, "SELECT apply_id,apply_generation "
                   "FROM manager_apply_scene_activated_records "
                   "ORDER BY apply_id,apply_generation LIMIT ?");
    scene_activated_records.Bind(
        1, static_cast<std::int64_t>(kMaximumManagerApplySceneActivatedRecords +
                                     1U));
    std::int64_t observed_scene_activated = 0;
    while (scene_activated_records.Step()) {
      ++observed_scene_activated;
      if (observed_scene_activated >
              static_cast<std::int64_t>(
                  kMaximumManagerApplySceneActivatedRecords) ||
          !SceneActivatedLocked(ApplyId(scene_activated_records.Text(0)),
                                scene_activated_records.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored scene-activation inventory is inconsistent");
      }
    }
    if (observed_activation_selections != activation_selection_count ||
        observed_scene_activated != scene_activated_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored activation inventory count is inconsistent");
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
    Statement retired_command_surfaces(
        database_, "SELECT apply_id,apply_generation "
                   "FROM manager_apply_retired_command_surfaces "
                   "ORDER BY apply_id,apply_generation LIMIT ?");
    retired_command_surfaces.Bind(
        1, static_cast<std::int64_t>(
               kMaximumManagerApplyRetiredCommandSurfaceRecords + 1U));
    std::int64_t observed_retired_command_surfaces = 0;
    while (retired_command_surfaces.Step()) {
      ++observed_retired_command_surfaces;
      if (observed_retired_command_surfaces >
              static_cast<std::int64_t>(
                  kMaximumManagerApplyRetiredCommandSurfaceRecords) ||
          !RetiredCommandSurfacesLocked(
               ApplyId(retired_command_surfaces.Text(0)),
               retired_command_surfaces.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored command-surface retirement inventory is inconsistent");
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
    Statement scene_finalized_records(
        database_, "SELECT apply_id,apply_generation "
                   "FROM manager_apply_scene_finalized_records "
                   "ORDER BY apply_id,apply_generation LIMIT ?");
    scene_finalized_records.Bind(
        1, static_cast<std::int64_t>(kMaximumManagerApplySceneFinalizedRecords +
                                     1U));
    std::int64_t observed_scene_finalized = 0;
    while (scene_finalized_records.Step()) {
      ++observed_scene_finalized;
      if (observed_scene_finalized >
              static_cast<std::int64_t>(
                  kMaximumManagerApplySceneFinalizedRecords) ||
          !SceneFinalizedLocked(ApplyId(scene_finalized_records.Text(0)),
                                scene_finalized_records.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored scene-Finalize inventory is inconsistent");
      }
    }
    if (observed_private_materializations != private_materialization_count ||
        observed_retired_command_surfaces != retired_command_surfaces_count ||
        observed_publication_receipts != publication_receipt_count ||
        observed_scene_finalized != scene_finalized_count) {
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
        scene_stage_intent_count,
        scene_staged_count,
        activation_selection_count,
        scene_activated_count,
        private_materialization_count,
        retired_command_surfaces_count,
        publication_receipt_count,
        scene_finalized_count,
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
  bool closed_{};
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
  const auto encoded = EncodeIntent(intent);
  if (intent.cell_id != impl_->cell_id_) {
    throw ManagerApplyJournalConflictError(
        "Apply intent belongs to a different cell");
  }
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());

  if (const auto existing =
          impl_->IntentLocked(intent.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->intent != intent ||
        existing->intent_digest.value() != encoded.intent_digest) {
      throw ManagerApplyJournalConflictError(
          "Apply identity conflicts with the durable intent");
    }
    transaction.Commit();
    return {*existing, false};
  }
  if (const auto existing_request =
          impl_->RequestLocked(intent.request_context.request_owner_epoch,
                               intent.request_context.request_id);
      existing_request.has_value()) {
    throw ManagerApplyJournalConflictError(
        "public request identity already names another Apply intent");
  }

  const auto high_water = impl_->GenerationHighWaterLocked(intent.apply_id);
  if (high_water.has_value() &&
      *high_water == std::numeric_limits<std::int64_t>::max()) {
    throw ManagerApplyJournalLimitError(
        "Apply generation high-water mark is exhausted");
  }
  const auto expected_generation = high_water.has_value() ? *high_water + 1 : 1;
  if (encoded.apply_generation != expected_generation) {
    throw ManagerApplyJournalConflictError(
        "Apply generation must advance its durable high-water mark by "
        "exactly one");
  }
  const auto intent_count = ScalarInteger(
      impl_->database_, "SELECT COUNT(*) FROM manager_apply_intents");
  if (intent_count >= static_cast<std::int64_t>(kMaximumManagerApplyIntents)) {
    throw ManagerApplyJournalLimitError(
        "Apply intent inventory reached its fixed limit");
  }

  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_intents ("
      "apply_id,apply_generation,request_id,request_owner_epoch,"
      "source_label,correlation_id,predecessor_retained_bundle_id,"
      "predecessor_graph_revision,predecessor_scene_id,"
      "predecessor_scene_revision_id,predecessor_scene_content_hash,"
      "candidate_retained_bundle_id,candidate_graph_revision,"
      "expected_admission_generation,parent_authoring_claim,intent_digest"
      ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
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
      7, intent.expected_active_projection_revision.retained_bundle_id.value());
  insert.Bind(
      8, intent.expected_active_projection_revision.graph_revision.value());
  insert.Bind(9, intent.expected_active_projection_revision.scene_revision
                     .scene_id.value());
  insert.Bind(10, encoded.predecessor_scene_revision_id);
  insert.Bind(11, intent.expected_active_projection_revision.scene_revision
                      .content_hash.value());
  insert.Bind(12, intent.candidate_retained_bundle_id.value());
  insert.Bind(13, intent.candidate_graph_revision.value());
  if (encoded.expected_admission_generation.has_value()) {
    insert.Bind(14, *encoded.expected_admission_generation);
  } else {
    insert.BindNull(14);
  }
  if (encoded.parent_authoring_claim.has_value()) {
    insert.BindBlob(15, *encoded.parent_authoring_claim);
  } else {
    insert.BindNull(15);
  }
  insert.Bind(16, encoded.intent_digest);
  static_cast<void>(insert.Step());

  const auto stored =
      impl_->IntentLocked(intent.apply_id, encoded.apply_generation);
  if (!stored.has_value() || stored->intent != intent ||
      stored->intent_digest.value() != encoded.intent_digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted Apply intent cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return {*stored, true};
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
  const auto result = impl_->IntentLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

std::vector<PersistedManagerApplyStatus>
SqliteManagerApplyJournal::ReadStatusInventory() const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");

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
    auto intent = impl_->IntentLocked(apply_id, generation);
    if (!intent.has_value()) {
      throw ManagerApplyJournalIntegrityError(
          "Apply status inventory references a missing intent");
    }
    result.push_back(PersistedManagerApplyStatus{
        std::move(*intent),
        impl_->AffectedScopeLocked(apply_id, generation),
        impl_->FenceAttemptLocked(apply_id, generation),
        impl_->FenceAcceptanceLocked(apply_id, generation),
        impl_->RuntimeConvergenceIntentLocked(apply_id, generation),
        impl_->LatestRuntimeStagedReadyLocked(apply_id, generation),
        impl_->SceneStageIntentLocked(apply_id, generation),
        impl_->SceneStagedLocked(apply_id, generation),
        impl_->ActivationSelectionLocked(apply_id, generation),
        impl_->SceneActivatedLocked(apply_id, generation),
        impl_->PrivateMaterializationLocked(apply_id, generation),
        impl_->RetiredCommandSurfacesLocked(apply_id, generation),
        impl_->PublicationReceiptLocked(apply_id, generation),
        impl_->SceneFinalizedLocked(apply_id, generation),
        impl_->FenceReleaseIntentLocked(apply_id, generation),
        impl_->FenceReleasedLocked(apply_id, generation),
        impl_->TerminalOutcomeLocked(apply_id, generation),
    });
  }
  transaction.Commit();
  return result;
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
      "busy_disposition,affected_scope_digest) "
      "VALUES (?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, affected_scope.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, intent_digest.value());
  insert.Bind(4, encoded.components);
  insert.Bind(5, encoded.domains);
  insert.Bind(6, encoded.bindings);
  insert.Bind(7, encoded.resources);
  insert.Bind(8, encoded.busy_disposition);
  insert.Bind(9, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->AffectedScopeLocked(affected_scope.apply_id,
                                                 encoded.apply_generation);
  if (!stored.has_value() ||
      stored->affected_scope_digest.value() != encoded.digest ||
      stored->affected_scope != affected_scope) {
    throw ManagerApplyJournalIntegrityError(
        "inserted Apply affected scope cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
      staged_ready.apply_id, encoded.apply_generation);
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  const auto result =
      impl_->LatestRuntimeStagedReadyLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplySceneStageIntentWriteResult
SqliteManagerApplyJournal::EnsureSceneStageIntent(
    const ManagerApplySceneStageIntentDTO &scene_stage_intent) {
  const auto encoded = EncodeSceneStageIntent(scene_stage_intent);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  if (const auto existing = impl_->SceneStageIntentLocked(
          scene_stage_intent.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->scene_stage_intent != scene_stage_intent ||
        existing->scene_stage_intent_digest.value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has a different immutable scene-stage "
          "intent");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto staged = impl_->RuntimeStagedReadyByDigestLocked(
      scene_stage_intent.apply_id, encoded.apply_generation,
      scene_stage_intent.runtime_staged_ready_digest);
  const auto latest_staged = impl_->LatestRuntimeStagedReadyLocked(
      scene_stage_intent.apply_id, encoded.apply_generation);
  const auto parent = impl_->IntentLocked(scene_stage_intent.apply_id,
                                          encoded.apply_generation);
  if (!staged.has_value() ||
      !latest_staged.has_value() || *latest_staged != *staged ||
      !WithdrawalsMatchRuntimeStage(scene_stage_intent, staged->staged_ready) ||
      staged->staged_ready.predecessor_publication_generation !=
          scene_stage_intent.predecessor_publication_generation ||
      staged->staged_ready.predecessor_public_pointer_digest !=
          scene_stage_intent.predecessor_public_pointer_digest ||
      !parent.has_value() ||
      parent->intent.expected_active_projection_revision !=
          scene_stage_intent.expected_active_projection_revision ||
      parent->intent.candidate_retained_bundle_id !=
          scene_stage_intent.candidate_retained_bundle_id ||
      parent->intent.candidate_graph_revision !=
          scene_stage_intent.candidate_graph_revision) {
    throw ManagerApplyJournalConflictError(
        "scene-stage intent does not name its exact latest staged-ready/Apply "
        "records");
  }
  const auto count =
      ScalarInteger(impl_->database_,
                    "SELECT COUNT(*) FROM manager_apply_scene_stage_intents");
  if (count >=
      static_cast<std::int64_t>(kMaximumManagerApplySceneStageIntents)) {
    throw ManagerApplyJournalLimitError(
        "scene-stage intent inventory reached its fixed limit");
  }
  const auto &predecessor =
      scene_stage_intent.expected_active_projection_revision;
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_scene_stage_intents ("
      "apply_id,apply_generation,runtime_staged_ready_digest,"
      "predecessor_publication_generation,"
      "predecessor_public_pointer_digest,"
      "predecessor_retained_bundle_id,predecessor_graph_revision,"
      "predecessor_scene_id,predecessor_scene_revision_id,"
      "predecessor_scene_content_hash,candidate_retained_bundle_id,"
      "candidate_graph_revision,native_transition_rows_encoding,"
      "scene_stage_intent_digest) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, scene_stage_intent.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, scene_stage_intent.runtime_staged_ready_digest.value());
  insert.Bind(4, encoded.predecessor_publication_generation);
  insert.Bind(5, scene_stage_intent.predecessor_public_pointer_digest.value());
  insert.Bind(6, predecessor.retained_bundle_id.value());
  insert.Bind(7, predecessor.graph_revision.value());
  insert.Bind(8, predecessor.scene_revision.scene_id.value());
  insert.Bind(9, encoded.predecessor_scene_revision_id);
  insert.Bind(10, predecessor.scene_revision.content_hash.value());
  insert.Bind(11, scene_stage_intent.candidate_retained_bundle_id.value());
  insert.Bind(12, scene_stage_intent.candidate_graph_revision.value());
  insert.Bind(13, encoded.transition_rows);
  insert.Bind(14, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->SceneStageIntentLocked(
      scene_stage_intent.apply_id, encoded.apply_generation);
  if (!stored.has_value() || stored->scene_stage_intent != scene_stage_intent ||
      stored->scene_stage_intent_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted scene-stage intent cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplySceneStageIntent>
SqliteManagerApplyJournal::ReadSceneStageIntent(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto result = impl_->SceneStageIntentLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplySceneStagedWriteResult SqliteManagerApplyJournal::EnsureSceneStaged(
    const ManagerApplySceneStagedDTO &scene_staged) {
  const auto encoded = EncodeSceneStaged(scene_staged);
  const auto &result = scene_staged.staged_result;
  const auto &candidate = *result.candidate_projection_revision;
  const auto &evidence = *result.transition_evidence;
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto intent =
      impl_->SceneStageIntentLocked(result.apply_id, encoded.apply_generation);
  bool rows_match =
      intent.has_value() &&
      evidence.transition_rows.size() ==
          intent->scene_stage_intent.native_transition_rows.size();
  if (rows_match) {
    for (std::size_t index = 0U; index < evidence.transition_rows.size();
         ++index) {
      if (evidence.transition_rows[index].transition_row !=
          intent->scene_stage_intent.native_transition_rows[index]) {
        rows_match = false;
        break;
      }
    }
  }
  if (!intent.has_value() ||
      intent->scene_stage_intent_digest !=
          scene_staged.scene_stage_intent_digest ||
      result.expected_active_projection_revision !=
          intent->scene_stage_intent.expected_active_projection_revision ||
      candidate.retained_bundle_id !=
          intent->scene_stage_intent.candidate_retained_bundle_id ||
      candidate.graph_revision !=
          intent->scene_stage_intent.candidate_graph_revision ||
      !rows_match) {
    throw ManagerApplyJournalConflictError(
        "scene staged evidence does not match its exact stage intent");
  }
  if (const auto existing =
          impl_->SceneStagedLocked(result.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (!SameManagerApplySceneStaged(existing->scene_staged, scene_staged) ||
        existing->scene_staged_digest.value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has different scene staged evidence");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto count =
      ScalarInteger(impl_->database_,
                    "SELECT COUNT(*) FROM manager_apply_scene_staged_records");
  if (count >=
      static_cast<std::int64_t>(kMaximumManagerApplySceneStagedRecords)) {
    throw ManagerApplyJournalLimitError(
        "scene staged-evidence inventory reached its fixed limit");
  }
  const auto &predecessor = result.expected_active_projection_revision;
  Statement insert(impl_->database_,
                   "INSERT INTO manager_apply_scene_staged_records ("
                   "apply_id,apply_generation,scene_stage_intent_digest,"
                   "predecessor_retained_bundle_id,predecessor_graph_revision,"
                   "predecessor_scene_id,predecessor_scene_revision_id,"
                   "predecessor_scene_content_hash,projection_stage_generation,"
                   "candidate_retained_bundle_id,candidate_graph_revision,"
                   "candidate_scene_id,candidate_scene_revision_id,"
                   "candidate_scene_content_hash,result_state,"
                   "transition_evidence_encoding,scene_staged_digest) "
                   "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, result.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, scene_staged.scene_stage_intent_digest.value());
  insert.Bind(4, predecessor.retained_bundle_id.value());
  insert.Bind(5, predecessor.graph_revision.value());
  insert.Bind(6, predecessor.scene_revision.scene_id.value());
  insert.Bind(7, encoded.predecessor_scene_revision_id);
  insert.Bind(8, predecessor.scene_revision.content_hash.value());
  insert.Bind(9, encoded.projection_stage_generation);
  insert.Bind(10, candidate.retained_bundle_id.value());
  insert.Bind(11, candidate.graph_revision.value());
  insert.Bind(12, candidate.scene_revision.scene_id.value());
  insert.Bind(13, encoded.candidate_scene_revision_id);
  insert.Bind(14, candidate.scene_revision.content_hash.value());
  insert.Bind(15, "staged");
  insert.Bind(16, encoded.transition_evidence);
  insert.Bind(17, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored =
      impl_->SceneStagedLocked(result.apply_id, encoded.apply_generation);
  if (!stored.has_value() ||
      !SameManagerApplySceneStaged(stored->scene_staged, scene_staged) ||
      stored->scene_staged_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted scene staged evidence cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplySceneStaged>
SqliteManagerApplyJournal::ReadSceneStaged(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto result = impl_->SceneStagedLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyActivationSelectionWriteResult
SqliteManagerApplyJournal::EnsureActivationSelection(
    const ManagerApplyActivationSelectionDTO &activation_selection) {
  const auto encoded = EncodeActivationSelection(activation_selection);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto staged = impl_->SceneStagedLocked(activation_selection.apply_id,
                                               encoded.apply_generation);
  if (!staged.has_value() ||
      staged->scene_staged_digest != activation_selection.scene_staged_digest ||
      staged->scene_staged.staged_result.expected_active_projection_revision !=
          activation_selection.predecessor_projection_revision ||
      *staged->scene_staged.staged_result.projection_stage_generation !=
          activation_selection.projection_stage_generation ||
      *staged->scene_staged.staged_result.candidate_projection_revision !=
          activation_selection.candidate_projection_revision) {
    throw ManagerApplyJournalConflictError(
        "activation selection does not match exact staged evidence");
  }
  if (const auto existing = impl_->ActivationSelectionLocked(
          activation_selection.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->activation_selection != activation_selection ||
        existing->activation_selection_digest.value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has a different activation selection");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto count =
      ScalarInteger(impl_->database_,
                    "SELECT COUNT(*) FROM manager_apply_activation_selections");
  if (count >=
      static_cast<std::int64_t>(kMaximumManagerApplyActivationSelections)) {
    throw ManagerApplyJournalLimitError(
        "activation-selection inventory reached its fixed limit");
  }
  const auto &predecessor =
      activation_selection.predecessor_projection_revision;
  const auto &candidate = activation_selection.candidate_projection_revision;
  Statement insert(impl_->database_,
                   "INSERT INTO manager_apply_activation_selections ("
                   "apply_id,apply_generation,scene_staged_digest,"
                   "predecessor_retained_bundle_id,predecessor_graph_revision,"
                   "predecessor_scene_id,predecessor_scene_revision_id,"
                   "predecessor_scene_content_hash,projection_stage_generation,"
                   "candidate_retained_bundle_id,candidate_graph_revision,"
                   "candidate_scene_id,candidate_scene_revision_id,"
                   "candidate_scene_content_hash,activation_selection_digest) "
                   "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, activation_selection.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, activation_selection.scene_staged_digest.value());
  insert.Bind(4, predecessor.retained_bundle_id.value());
  insert.Bind(5, predecessor.graph_revision.value());
  insert.Bind(6, predecessor.scene_revision.scene_id.value());
  insert.Bind(7, encoded.predecessor_scene_revision_id);
  insert.Bind(8, predecessor.scene_revision.content_hash.value());
  insert.Bind(9, encoded.projection_stage_generation);
  insert.Bind(10, candidate.retained_bundle_id.value());
  insert.Bind(11, candidate.graph_revision.value());
  insert.Bind(12, candidate.scene_revision.scene_id.value());
  insert.Bind(13, encoded.candidate_scene_revision_id);
  insert.Bind(14, candidate.scene_revision.content_hash.value());
  insert.Bind(15, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->ActivationSelectionLocked(
      activation_selection.apply_id, encoded.apply_generation);
  if (!stored.has_value() ||
      stored->activation_selection != activation_selection ||
      stored->activation_selection_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted activation selection cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyActivationSelection>
SqliteManagerApplyJournal::ReadActivationSelection(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto result = impl_->ActivationSelectionLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplySceneActivatedWriteResult
SqliteManagerApplyJournal::EnsureSceneActivated(
    const ManagerApplySceneActivatedDTO &scene_activated) {
  const auto encoded = EncodeSceneActivated(scene_activated);
  const auto &result = scene_activated.activation_result;
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto selection = impl_->ActivationSelectionLocked(
      result.apply_id, encoded.apply_generation);
  if (!selection.has_value() ||
      selection->activation_selection_digest !=
          scene_activated.activation_selection_digest ||
      selection->activation_selection.predecessor_projection_revision !=
          result.expected_active_projection_revision ||
      selection->activation_selection.projection_stage_generation !=
          result.projection_stage_generation ||
      selection->activation_selection.candidate_projection_revision !=
          result.candidate_projection_revision) {
    throw ManagerApplyJournalConflictError(
        "scene activation evidence does not match exact selection");
  }
  const auto staged =
      impl_->SceneStagedLocked(result.apply_id, encoded.apply_generation);
  bool rows_match = staged.has_value() &&
                    result.transition_evidence.transition_rows.size() ==
                        staged->scene_staged.staged_result.transition_evidence
                            ->transition_rows.size();
  if (rows_match) {
    for (std::size_t index = 0U;
         index < result.transition_evidence.transition_rows.size(); ++index) {
      if (result.transition_evidence.transition_rows[index].transition_row !=
          staged->scene_staged.staged_result.transition_evidence
              ->transition_rows[index]
              .transition_row) {
        rows_match = false;
        break;
      }
    }
  }
  if (!rows_match) {
    throw ManagerApplyJournalConflictError(
        "scene activation transition set differs from staged evidence");
  }
  if (const auto existing = impl_->SceneActivatedLocked(
          result.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (!SameManagerApplySceneActivated(existing->scene_activated,
                                        scene_activated) ||
        existing->scene_activated_digest.value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has different scene activation "
          "evidence");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto count = ScalarInteger(
      impl_->database_,
      "SELECT COUNT(*) FROM manager_apply_scene_activated_records");
  if (count >=
      static_cast<std::int64_t>(kMaximumManagerApplySceneActivatedRecords)) {
    throw ManagerApplyJournalLimitError(
        "scene activation-evidence inventory reached its fixed limit");
  }
  const auto &predecessor = result.expected_active_projection_revision;
  const auto &candidate = result.candidate_projection_revision;
  Statement insert(impl_->database_,
                   "INSERT INTO manager_apply_scene_activated_records ("
                   "apply_id,apply_generation,activation_selection_digest,"
                   "predecessor_retained_bundle_id,predecessor_graph_revision,"
                   "predecessor_scene_id,predecessor_scene_revision_id,"
                   "predecessor_scene_content_hash,projection_stage_generation,"
                   "candidate_retained_bundle_id,candidate_graph_revision,"
                   "candidate_scene_id,candidate_scene_revision_id,"
                   "candidate_scene_content_hash,result_state,"
                   "transition_evidence_encoding,scene_activated_digest) "
                   "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, result.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, scene_activated.activation_selection_digest.value());
  insert.Bind(4, predecessor.retained_bundle_id.value());
  insert.Bind(5, predecessor.graph_revision.value());
  insert.Bind(6, predecessor.scene_revision.scene_id.value());
  insert.Bind(7, encoded.predecessor_scene_revision_id);
  insert.Bind(8, predecessor.scene_revision.content_hash.value());
  insert.Bind(9, encoded.projection_stage_generation);
  insert.Bind(10, candidate.retained_bundle_id.value());
  insert.Bind(11, candidate.graph_revision.value());
  insert.Bind(12, candidate.scene_revision.scene_id.value());
  insert.Bind(13, encoded.candidate_scene_revision_id);
  insert.Bind(14, candidate.scene_revision.content_hash.value());
  insert.Bind(15, "activated");
  insert.Bind(16, encoded.transition_evidence);
  insert.Bind(17, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored =
      impl_->SceneActivatedLocked(result.apply_id, encoded.apply_generation);
  if (!stored.has_value() ||
      !SameManagerApplySceneActivated(stored->scene_activated,
                                      scene_activated) ||
      stored->scene_activated_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted scene activation evidence cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplySceneActivated>
SqliteManagerApplyJournal::ReadSceneActivated(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto result = impl_->SceneActivatedLocked(apply_id, generation);
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto activated = impl_->SceneActivatedLocked(
      private_materialization.apply_id, encoded.apply_generation);
  if (!activated.has_value() ||
      activated->scene_activated_digest !=
          private_materialization.scene_activated_digest ||
      activated->scene_activated.activation_result
              .projection_stage_generation !=
          private_materialization.projection_stage_generation ||
      activated->scene_activated.activation_result
              .candidate_projection_revision !=
          private_materialization.candidate_projection_revision) {
    throw ManagerApplyJournalConflictError(
        "private materialization does not match exact scene activation");
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
                   "apply_id,apply_generation,scene_activated_digest,"
                   "projection_stage_generation,candidate_retained_bundle_id,"
                   "candidate_graph_revision,candidate_scene_id,"
                   "candidate_scene_revision_id,candidate_scene_content_hash,"
                   "retained_bundle_canonical_content_digest,"
                   "retained_bundle_canonical_byte_size,"
                   "private_materialization_content_digest) "
                   "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, private_materialization.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, private_materialization.scene_activated_digest.value());
  insert.Bind(4, encoded.projection_stage_generation);
  insert.Bind(5, candidate.retained_bundle_id.value());
  insert.Bind(6, candidate.graph_revision.value());
  insert.Bind(7, candidate.scene_revision.scene_id.value());
  insert.Bind(8, encoded.candidate_scene_revision_id);
  insert.Bind(9, candidate.scene_revision.content_hash.value());
  insert.Bind(
      10,
      private_materialization.retained_bundle_canonical_content_digest.value());
  insert.Bind(11, encoded.retained_bundle_canonical_byte_size);
  insert.Bind(12, encoded.digest);
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  const auto result = impl_->PrivateMaterializationLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplyObsoleteCommandSurfacesRetiredWriteResult
SqliteManagerApplyJournal::EnsureObsoleteCommandSurfacesRetired(
    const ManagerApplyObsoleteCommandSurfacesRetiredDTO
        &retired_command_surfaces) {
  const auto encoded = EncodeRetiredCommandSurfaces(retired_command_surfaces);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto materialization = impl_->PrivateMaterializationLocked(
      retired_command_surfaces.apply_id, encoded.apply_generation);
  if (!materialization.has_value() ||
      materialization->private_materialization_content_digest !=
          retired_command_surfaces.private_materialization_content_digest) {
    throw ManagerApplyJournalConflictError(
        "command-surface retirement does not match exact private "
        "materialization");
  }
  if (const auto existing = impl_->RetiredCommandSurfacesLocked(
          retired_command_surfaces.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (existing->retired_command_surfaces != retired_command_surfaces ||
        existing->obsolete_managed_command_surfaces_retired_evidence_digest
                .value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has different command-surface "
          "retirement evidence");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto count = ScalarInteger(
      impl_->database_,
      "SELECT COUNT(*) FROM manager_apply_retired_command_surfaces");
  if (count >= static_cast<std::int64_t>(
                   kMaximumManagerApplyRetiredCommandSurfaceRecords)) {
    throw ManagerApplyJournalLimitError(
        "command-surface retirement inventory reached its fixed limit");
  }
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_retired_command_surfaces ("
      "apply_id,apply_generation,private_materialization_content_digest,"
      "command_surfaces_encoding,retired_command_surfaces_evidence_digest) "
      "VALUES (?,?,?,?,?)");
  insert.Bind(1, retired_command_surfaces.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(
      3,
      retired_command_surfaces.private_materialization_content_digest.value());
  insert.Bind(4, encoded.command_surfaces);
  insert.Bind(5, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->RetiredCommandSurfacesLocked(
      retired_command_surfaces.apply_id, encoded.apply_generation);
  if (!stored.has_value() ||
      stored->retired_command_surfaces != retired_command_surfaces ||
      stored->obsolete_managed_command_surfaces_retired_evidence_digest
              .value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted command-surface retirement evidence cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplyObsoleteCommandSurfacesRetired>
SqliteManagerApplyJournal::ReadObsoleteCommandSurfacesRetired(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto result = impl_->RetiredCommandSurfacesLocked(apply_id, generation);
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto retired = impl_->RetiredCommandSurfacesLocked(
      named->apply_id, encoded.apply_generation);
  if (!retired.has_value() ||
      retired->obsolete_managed_command_surfaces_retired_evidence_digest !=
          publication
              .obsolete_managed_command_surfaces_retired_evidence_digest ||
      retired->retired_command_surfaces
              .private_materialization_content_digest !=
          request.private_materialization_content_digest) {
    throw ManagerApplyJournalConflictError(
        "publication receipt does not match exact retirement evidence");
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
      "retired_command_surfaces_evidence_digest,"
      "projection_stage_generation,candidate_retained_bundle_id,"
      "candidate_graph_revision,candidate_scene_id,"
      "candidate_scene_revision_id,candidate_scene_content_hash,"
      "expected_previous_publication_generation,"
      "private_materialization_content_digest,publication_generation,"
      "durable_public_pointer_content_digest,publication_receipt_digest) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, named->apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(
      3, publication.obsolete_managed_command_surfaces_retired_evidence_digest
             .value());
  insert.Bind(4, encoded.projection_stage_generation);
  insert.Bind(5, candidate.retained_bundle_id.value());
  insert.Bind(6, candidate.graph_revision.value());
  insert.Bind(7, candidate.scene_revision.scene_id.value());
  insert.Bind(8, encoded.candidate_scene_revision_id);
  insert.Bind(9, candidate.scene_revision.content_hash.value());
  insert.Bind(10, encoded.expected_previous_publication_generation);
  insert.Bind(11, request.private_materialization_content_digest.value());
  insert.Bind(12, encoded.publication_generation);
  insert.Bind(13, receipt.durable_public_pointer_content_digest.value());
  insert.Bind(14, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->PublicationReceiptLocked(named->apply_id,
                                                      encoded.apply_generation);
  if (!stored.has_value() || stored->publication != publication ||
      stored->publication_receipt_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted Apply publication receipt cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  const auto result = impl_->PublicationReceiptLocked(apply_id, generation);
  transaction.Commit();
  return result;
}

ManagerApplySceneFinalizedWriteResult
SqliteManagerApplyJournal::EnsureSceneFinalized(
    const ManagerApplySceneFinalizedDTO &scene_finalized) {
  const auto encoded = EncodeSceneFinalized(scene_finalized);
  const auto &result = scene_finalized.finalized_result;
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto publication = impl_->PublicationReceiptLocked(
      result.apply_id, encoded.apply_generation);
  if (!publication.has_value() ||
      publication->publication_receipt_digest !=
          scene_finalized.publication_receipt_digest ||
      publication->publication.publication_receipt !=
          result.publication_receipt) {
    throw ManagerApplyJournalConflictError(
        "scene Finalize evidence does not match exact publication receipt");
  }
  const auto activated =
      impl_->SceneActivatedLocked(result.apply_id, encoded.apply_generation);
  bool rows_match = activated.has_value() &&
                    activated->scene_activated.activation_result
                            .projection_stage_generation ==
                        result.projection_stage_generation &&
                    activated->scene_activated.activation_result
                            .candidate_projection_revision ==
                        result.candidate_projection_revision &&
                    activated->scene_activated.activation_result
                            .transition_evidence.transition_rows.size() ==
                        result.transition_evidence.transition_rows.size();
  if (rows_match) {
    for (std::size_t index = 0U;
         index < result.transition_evidence.transition_rows.size(); ++index) {
      if (activated->scene_activated.activation_result.transition_evidence
              .transition_rows[index]
              .transition_row !=
          result.transition_evidence.transition_rows[index].transition_row) {
        rows_match = false;
        break;
      }
    }
  }
  if (!rows_match) {
    throw ManagerApplyJournalConflictError(
        "scene Finalize transition set differs from activation evidence");
  }
  if (const auto existing = impl_->SceneFinalizedLocked(
          result.apply_id, encoded.apply_generation);
      existing.has_value()) {
    if (!SameManagerApplySceneFinalized(existing->scene_finalized,
                                        scene_finalized) ||
        existing->scene_finalized_digest.value() != encoded.digest) {
      throw ManagerApplyJournalConflictError(
          "Apply generation already has different scene Finalize evidence");
    }
    transaction.Commit();
    return {*existing, false};
  }
  const auto count = ScalarInteger(
      impl_->database_,
      "SELECT COUNT(*) FROM manager_apply_scene_finalized_records");
  if (count >=
      static_cast<std::int64_t>(kMaximumManagerApplySceneFinalizedRecords)) {
    throw ManagerApplyJournalLimitError(
        "scene-Finalize inventory reached its fixed limit");
  }
  const auto &candidate = result.candidate_projection_revision;
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_scene_finalized_records ("
      "apply_id,apply_generation,publication_receipt_digest,"
      "projection_stage_generation,candidate_retained_bundle_id,"
      "candidate_graph_revision,candidate_scene_id,"
      "candidate_scene_revision_id,candidate_scene_content_hash,"
      "transition_evidence_encoding,candidate_aggregate_health_encoding,"
      "scene_finalized_digest) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, result.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, scene_finalized.publication_receipt_digest.value());
  insert.Bind(4, encoded.projection_stage_generation);
  insert.Bind(5, candidate.retained_bundle_id.value());
  insert.Bind(6, candidate.graph_revision.value());
  insert.Bind(7, candidate.scene_revision.scene_id.value());
  insert.Bind(8, encoded.candidate_scene_revision_id);
  insert.Bind(9, candidate.scene_revision.content_hash.value());
  insert.Bind(10, encoded.transition_evidence);
  insert.Bind(11, encoded.aggregate_health);
  insert.Bind(12, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored =
      impl_->SceneFinalizedLocked(result.apply_id, encoded.apply_generation);
  if (!stored.has_value() ||
      !SameManagerApplySceneFinalized(stored->scene_finalized,
                                      scene_finalized) ||
      stored->scene_finalized_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted scene Finalize evidence cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return {*stored, true};
}

std::optional<PersistedManagerApplySceneFinalized>
SqliteManagerApplyJournal::ReadSceneFinalized(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto generation =
      CheckedSigned(apply_generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto result = impl_->SceneFinalizedLocked(apply_id, generation);
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto finalized =
      impl_->SceneFinalizedLocked(owner.apply_id, encoded.apply_generation);
  const auto acceptance =
      impl_->FenceAcceptanceLocked(owner.apply_id, encoded.apply_generation);
  const auto publication =
      impl_->PublicationReceiptLocked(owner.apply_id, encoded.apply_generation);
  const auto &evidence =
      fence_release_intent.release_request.reconciliation_evidence;
  if (!finalized.has_value() ||
      finalized->scene_finalized_digest !=
          fence_release_intent.scene_finalized_digest ||
      !acceptance.has_value() ||
      acceptance->acceptance_digest !=
          fence_release_intent.fence_acceptance_digest ||
      acceptance->accepted_fence.configuration_fence_id !=
          evidence.configuration_fence_id ||
      acceptance->accepted_fence.configuration_fence_generation !=
          evidence.configuration_fence_generation ||
      acceptance->accepted_fence.captured_scope != evidence.reconciled_scope ||
      finalized->scene_finalized.finalized_result
              .candidate_projection_revision !=
          evidence.active_projection_revision ||
      !publication.has_value() ||
      publication->publication.publication_receipt.publication_generation !=
          evidence.active_publication_generation ||
      publication->publication.publication_receipt
              .durable_public_pointer_content_digest !=
          evidence.active_public_pointer_digest) {
    throw ManagerApplyJournalConflictError(
        "fence-release intent does not match exact finalized, fence, and "
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
                   "apply_id,apply_generation,scene_finalized_digest,"
                   "fence_acceptance_digest,release_request_encoding,"
                   "fence_release_intent_digest) VALUES (?,?,?,?,?,?)");
  insert.Bind(1, owner.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, fence_release_intent.scene_finalized_digest.value());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto released = impl_->FenceReleasedLocked(terminal_outcome.apply_id,
                                                   encoded.apply_generation);
  const auto finalized = impl_->SceneFinalizedLocked(terminal_outcome.apply_id,
                                                     encoded.apply_generation);
  const auto publication = impl_->PublicationReceiptLocked(
      terminal_outcome.apply_id, encoded.apply_generation);
  if (!released.has_value() ||
      released->fence_released_digest !=
          terminal_outcome.fence_released_digest ||
      !finalized.has_value() ||
      finalized->scene_finalized_digest !=
          terminal_outcome.scene_finalized_digest ||
      finalized->scene_finalized.finalized_result
              .candidate_projection_revision !=
          terminal_outcome.candidate_projection_revision ||
      !publication.has_value() ||
      publication->publication_receipt_digest !=
          terminal_outcome.publication_receipt_digest) {
    throw ManagerApplyJournalConflictError(
        "terminal Apply outcome does not match exact completion evidence");
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
  const auto &candidate = terminal_outcome.candidate_projection_revision;
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_terminal_outcomes ("
      "apply_id,apply_generation,fence_released_digest,"
      "scene_finalized_digest,publication_receipt_digest,"
      "candidate_retained_bundle_id,candidate_graph_revision,"
      "candidate_scene_id,candidate_scene_revision_id,"
      "candidate_scene_content_hash,outcome_state,terminal_outcome_digest) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, terminal_outcome.apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, terminal_outcome.fence_released_digest.value());
  insert.Bind(4, terminal_outcome.scene_finalized_digest.value());
  insert.Bind(5, terminal_outcome.publication_receipt_digest.value());
  insert.Bind(6, candidate.retained_bundle_id.value());
  insert.Bind(7, candidate.graph_revision.value());
  insert.Bind(8, candidate.scene_revision.scene_id.value());
  insert.Bind(9, encoded.candidate_scene_revision_id);
  insert.Bind(10, candidate.scene_revision.content_hash.value());
  insert.Bind(11, encoded.state);
  insert.Bind(12, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->TerminalOutcomeLocked(terminal_outcome.apply_id,
                                                   encoded.apply_generation);
  if (!stored.has_value() || stored->terminal_outcome != terminal_outcome ||
      stored->terminal_outcome_digest.value() != encoded.digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted terminal Apply outcome cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  const auto result = impl_->TerminalOutcomeLocked(apply_id, generation);
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
    Transaction snapshot(database, "BEGIN");
    ValidateApplyJournalIdentityAndSchema(database, cell_id);
    Statement row(
        database,
        "SELECT t.terminal_outcome_digest FROM manager_apply_intents i "
        "JOIN manager_apply_terminal_outcomes t "
        "ON t.apply_id=i.apply_id AND "
        "t.apply_generation=i.apply_generation "
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
    const auto digest = found ? std::optional<std::string>(row.Text(0))
                              : std::nullopt;
    const bool duplicate = found && row.Step();
    snapshot.Commit();
    sqlite3_close(database);
    database = nullptr;
    if (!digest.has_value() || duplicate ||
        *digest != release.settlement.evidence_digest.value()) {
      return rejected("manager_apply_terminal_evidence_mismatch");
    }
    return core::Result<void>::Ok();
  } catch (const std::exception &) {
    sqlite3_close(database);
    return rejected("manager_apply_journal_read_failed");
  }
}

} // namespace manyforge::component_manager
