#include "manyforge_component_manager/manager_apply_journal.hpp"

#include <manyforge_deployment/apply_fence_qualification.hpp>

#include <openssl/evp.h>
#include <sqlite3.h>

#include <manyforge_persistence/sqlite_read_only_preflight.hpp>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>

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
    "manyforge.component-manager-terminal-outcome.ua4.v3";
constexpr int kBusyTimeoutMilliseconds = 10000;
constexpr std::int64_t kWalCheckpointTargetBytes = 16LL * 1024LL * 1024LL;
constexpr std::int64_t kExpectedSchemaObjectCardinality = 42;

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
           core::detail::EncodeConfigurationFenceScopeRecord(scope);
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

std::string EncodeExpectedConfigurations(
    const std::vector<DomainActiveControlConfigurationDTO> &configurations,
    const std::vector<core::ProviderConfigurationFenceLineageDTO> &lineages) {
  if (lineages.empty())
    return EncodeActiveConfigurations(configurations);
  if (lineages.size() > kMaximumManagerApplyFenceDomains)
    throw ManagerApplyJournalLimitError(
        "provider lineage count exceeds the qualified bound");
  std::vector<std::string> tokens;
  tokens.reserve(lineages.size() * 3U);
  for (const auto &lineage : lineages) {
    if (!lineage.valid())
      throw std::invalid_argument("provider lineage is invalid");
    ValidateBoundedIdentity(lineage.control_domain_id.value(),
                            "provider_lineage_domain");
    ValidateBoundedIdentity(lineage.provider_runtime_epoch.value(),
                            "provider_lineage_epoch");
    tokens.push_back(lineage.control_domain_id.value());
    tokens.push_back(lineage.provider_runtime_epoch.value());
    tokens.push_back(std::to_string(
        CheckedSigned(lineage.component_lifecycle_generation.value(),
                      "provider_lineage_generation")));
  }
  return "MF-PROVIDER-CONFIGURATION-LINEAGES-V1:" +
         EncodeTokens({EncodeActiveConfigurations(configurations),
                       EncodeTokens(tokens)});
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
    auto scope = core::detail::DecodeConfigurationFenceScopeRecord(
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

struct DecodedExpectedConfigurations final {
  std::vector<DomainActiveControlConfigurationDTO> configurations;
  std::vector<core::ProviderConfigurationFenceLineageDTO> lineages;
};
DecodedExpectedConfigurations
DecodeExpectedConfigurations(const std::string &encoding) {
  constexpr std::string_view prefix = "MF-PROVIDER-CONFIGURATION-LINEAGES-V1:";
  if (!encoding.starts_with(prefix))
    return {DecodeActiveConfigurations(encoding), {}};
  const auto fields = DecodeTokens(encoding.substr(prefix.size()), 2U,
                                   "provider configuration facts");
  if (fields.size() != 2U)
    throw ManagerApplyJournalIntegrityError(
        "provider configuration facts are malformed");
  DecodedExpectedConfigurations result{DecodeActiveConfigurations(fields[0]),
                                       {}};
  const auto tokens = DecodeTokens(
      fields[1], kMaximumManagerApplyFenceDomains * 3U, "provider lineages");
  if (tokens.empty() || tokens.size() % 3U != 0U)
    throw ManagerApplyJournalIntegrityError(
        "provider lineage tuple cardinality is invalid");
  for (std::size_t i = 0U; i < tokens.size(); i += 3U) {
    const auto generation =
        ParseCanonicalUnsigned(tokens[i + 2U], "provider lineage generation");
    if (generation == 0U ||
        generation > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()))
      throw ManagerApplyJournalIntegrityError(
          "provider lineage generation is invalid");
    result.lineages.push_back({ControlDomainId(tokens[i]),
                               RuntimeEpoch(tokens[i + 1U]),
                               core::ComponentLifecycleGeneration(generation)});
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
    AppendField(record, "intent_digest", failure.intent_digest.value());
    AppendField(record, "failure_phase", ToString(failure.phase));
    AppendField(record, "failure_code", failure.failure_code);
    AppendField(record, "failure_message", failure.failure_message);
    AppendField(record, "effect_disposition",
                ToString(failure.effect_disposition));
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
         lhs.reason == rhs.reason &&
         lhs.expected_provider_lineages == rhs.expected_provider_lineages;
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
      EncodeExpectedConfigurations(
          intent.expected_active_control_configurations,
          intent.expected_provider_lineages),
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

const char *BringUpPhaseName(ManagerApplyBringUpChildPhase phase) {
  switch (phase) {
  case ManagerApplyBringUpChildPhase::kAccepted:
    return "accepted";
  case ManagerApplyBringUpChildPhase::kHolding:
    return "holding";
  case ManagerApplyBringUpChildPhase::kTerminal:
    return "terminal";
  }
  throw std::invalid_argument("unknown configured bring-up child phase");
}

ManagerApplyBringUpChildPhase ParseBringUpPhase(std::string_view phase) {
  if (phase == "accepted")
    return ManagerApplyBringUpChildPhase::kAccepted;
  if (phase == "holding")
    return ManagerApplyBringUpChildPhase::kHolding;
  if (phase == "terminal")
    return ManagerApplyBringUpChildPhase::kTerminal;
  throw ManagerApplyJournalIntegrityError(
      "unknown stored bring-up child phase");
}

std::string EncodeBringUpProjection(const ProjectionRevisionDTO &revision) {
  return EncodeTokens(
      {revision.retained_bundle_id.value(), revision.graph_revision.value(),
       revision.scene_revision.scene_id.value(),
       std::to_string(CheckedSigned(revision.scene_revision.revision_id,
                                    "scene_revision")),
       revision.scene_revision.content_hash.value()});
}

ProjectionRevisionDTO DecodeBringUpProjection(std::string_view bytes) {
  const auto fields = DecodeTokens(bytes, 5U, "bring-up projection");
  if (fields.size() != 5U) {
    throw ManagerApplyJournalIntegrityError("stored bring-up projection shape");
  }
  return {RetainedBundleId(fields[0]),
          GraphRevision(fields[1]),
          {SceneId(fields[2]),
           ParseCanonicalSqliteUnsigned(fields[3], "scene_revision", true),
           ContentDigest(fields[4])}};
}

std::string
EncodeBringUpChild(const ManagerApplyBringUpChildRequestDTO &child) {
  if (!child.valid())
    throw std::invalid_argument("invalid bring-up child request");
  return EncodeTokens(
      {core::EncodeComponentLifecycleIntent(child.intent),
       EncodeBringUpProjection(child.candidate_projection_revision),
       child.candidate_retained_bundle_digest.value(),
       std::to_string(CheckedSigned(child.candidate_retained_bundle_bytes,
                                    "candidate_bundle_bytes")),
       child.runtime_unit_id.value(), child.runtime_epoch.value(),
       child.manager_runtime_epoch.value()});
}

ManagerApplyBringUpChildRequestDTO DecodeBringUpChild(std::string_view bytes) {
  const auto fields = DecodeTokens(bytes, 7U, "bring-up child");
  if (fields.size() != 7U) {
    throw ManagerApplyJournalIntegrityError("stored bring-up child shape");
  }
  return {
      core::DecodeComponentLifecycleIntent(fields[0]),
      DecodeBringUpProjection(fields[1]),
      ContentDigest(fields[2]),
      ParseCanonicalSqliteUnsigned(fields[3], "candidate_bundle_bytes", true),
      RuntimeUnitId(fields[4]),
      RuntimeEpoch(fields[5]),
      RuntimeEpoch(fields[6])};
}

std::string EncodeBringUpPlan(const ManagerApplyBringUpPlanDTO &plan) {
  if (!plan.valid())
    throw std::invalid_argument("invalid configured bring-up plan");
  std::vector<std::string> children;
  for (const auto &child : plan.children)
    children.push_back(EncodeBringUpChild(child));
  const auto &publication = plan.expected_publication;
  const auto &owner = std::get<NamedApplyPublicationTransactionDTO>(
      publication.transaction_ref);
  return EncodeTokens(
      {plan.apply_id.value(),
       std::to_string(
           CheckedSigned(plan.apply_generation.value(), "apply_generation")),
       plan.parent_intent_digest.value(),
       EncodeComponents(plan.configured_components), EncodeTokens(children),
       EncodeTokens(
           {owner.apply_id.value(),
            std::to_string(CheckedSigned(owner.apply_generation.value(),
                                         "apply_generation")),
            EncodeBringUpProjection(publication.candidate_projection_revision),
            std::to_string(CheckedSigned(
                publication.expected_previous_publication_generation->value(),
                "previous_publication_generation")),
            publication.private_materialization_content_digest.value()}),
       plan.predecessor_public_pointer_digest.value()});
}

ManagerApplyBringUpPlanDTO DecodeBringUpPlan(std::string_view bytes) {
  const auto fields = DecodeTokens(bytes, 7U, "bring-up plan");
  if (fields.size() != 7U)
    throw ManagerApplyJournalIntegrityError("stored bring-up plan shape");
  const auto components = DecodeTokens(
      fields[3], kMaximumManagerApplyFenceComponents, "configured components");
  std::vector<ControllableComponentId> configured;
  for (const auto &component : components)
    configured.emplace_back(component);
  const auto child_values = DecodeTokens(
      fields[4], kMaximumManagerApplyFenceComponents, "bring-up children");
  std::vector<ManagerApplyBringUpChildRequestDTO> children;
  for (const auto &child : child_values)
    children.push_back(DecodeBringUpChild(child));
  const auto publication = DecodeTokens(fields[5], 5U, "bring-up publication");
  if (publication.size() != 5U)
    throw ManagerApplyJournalIntegrityError(
        "stored bring-up publication shape");
  auto candidate = DecodeBringUpProjection(publication[2]);
  return {ApplyId(fields[0]),
          ApplyGeneration(ParseCanonicalSqliteUnsigned(
              fields[1], "apply_generation", true)),
          ContentDigest(fields[2]),
          std::move(configured),
          std::move(children),
          {NamedApplyPublicationTransactionDTO{
               ApplyId(publication[0]),
               ApplyGeneration(ParseCanonicalSqliteUnsigned(
                   publication[1], "apply_generation", true))},
           candidate, candidate.retained_bundle_id, candidate.graph_revision,
           ActiveBundlePublicationGeneration(ParseCanonicalSqliteUnsigned(
               publication[3], "previous_publication_generation", true)),
           ContentDigest(publication[4])},
          ContentDigest(fields[6])};
}

std::string BringUpPlanDigest(std::string_view encoding) {
  std::string record;
  AppendField(record, "domain",
              "manyforge.component-manager-apply-bring-up-plan.v1");
  AppendField(record, "plan", encoding);
  return RawDigest(record);
}

std::string BringUpProofDigest(const ManagerApplyBringUpChildProofDTO &proof,
                               std::string_view result_encoding) {
  std::string record;
  AppendField(record, "domain",
              "manyforge.component-manager-apply-bring-up-child-proof.v1");
  AppendField(record, "apply_id", proof.apply_id.value());
  AppendField(record, "apply_generation",
              std::to_string(CheckedSigned(proof.apply_generation.value(),
                                           "apply_generation")));
  AppendField(record, "plan_digest", proof.plan_digest.value());
  AppendField(record, "component_id", proof.controllable_component_id.value());
  AppendField(record, "phase", BringUpPhaseName(proof.phase));
  AppendField(record, "result", result_encoding);
  return RawDigest(record);
}

bool BringUpProofSucceeded(const ManagerApplyBringUpChildProofDTO &proof) {
  return proof.phase == ManagerApplyBringUpChildPhase::kTerminal &&
         proof.result.status.has_value() &&
         proof.result.status->phase ==
             core::ComponentLifecycleTransactionPhaseDTO::kSucceeded &&
         proof.result.status->terminal_result ==
             core::ComponentLifecycleTerminalResultDTO::kSucceeded &&
         !proof.result.status->reconciliation_required &&
         !proof.result.status->error.has_value();
}

void ValidateBringUpProof(
    const ManagerApplyBringUpPlanDTO &plan, const ContentDigest &plan_digest,
    const ManagerApplyBringUpChildProofDTO &proof,
    const std::vector<PersistedManagerApplyBringUpChildProof> &previous) {
  const auto child = std::find_if(
      plan.children.begin(), plan.children.end(), [&](const auto &value) {
        return value.intent.controllable_component_id ==
               proof.controllable_component_id;
      });
  if (proof.apply_id != plan.apply_id ||
      proof.apply_generation != plan.apply_generation ||
      proof.plan_digest != plan_digest || child == plan.children.end() ||
      !proof.result.locally_valid() ||
      proof.result.incident_recovery.has_value()) {
    throw ManagerApplyJournalIntegrityError(
        "bring-up proof does not match its exact original child");
  }
  static_cast<void>(BringUpPhaseName(proof.phase));
  if (!proof.result.acceptance.has_value()) {
    if (proof.phase != ManagerApplyBringUpChildPhase::kAccepted ||
        !proof.result.rejection.has_value() ||
        proof.result.status.has_value()) {
      throw ManagerApplyJournalIntegrityError(
          "bring-up no-admission result is not an exact rejection");
    }
    return;
  }
  if (!proof.result.status.has_value()) {
    throw ManagerApplyJournalIntegrityError(
        "bring-up acceptance lacks its owner status");
  }
  const auto &status = *proof.result.status;
  const auto &accepted = *proof.result.acceptance;
  if (!status.locally_valid() || status.acceptance != accepted ||
      !status.accepted_intent.replay_equal(child->intent) ||
      status.selected_bring_up.has_value() ||
      !status.apply_bring_up.has_value()) {
    throw ManagerApplyJournalIntegrityError(
        "bring-up owner status does not echo the original delegated request");
  }
  const auto &evidence = *status.apply_bring_up;
  const auto &authority = evidence.parent_authority;
  if (!authority.valid() || authority.parent.owner.apply_id != plan.apply_id ||
      authority.parent.owner.apply_generation != plan.apply_generation ||
      authority.parent.plan_digest != plan_digest ||
      evidence.candidate_projection_revision !=
          child->candidate_projection_revision ||
      evidence.candidate_retained_bundle_digest !=
          child->candidate_retained_bundle_digest ||
      evidence.candidate_retained_bundle_bytes !=
          child->candidate_retained_bundle_bytes ||
      evidence.original_execution_host.runtime_unit_id !=
          child->runtime_unit_id ||
      evidence.original_execution_host.runtime_epoch != child->runtime_epoch ||
      authority.held_fence_query.manager_runtime_epoch !=
          child->manager_runtime_epoch ||
      evidence.selected_scope.controllable_component_ids.size() != 1U ||
      evidence.selected_scope.controllable_component_ids.front() !=
          proof.controllable_component_id ||
      evidence.original_request_payload_digest !=
          core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
              child->intent, child->candidate_projection_revision,
              child->candidate_retained_bundle_digest,
              child->candidate_retained_bundle_bytes, child->runtime_unit_id,
              child->runtime_epoch, child->manager_runtime_epoch,
              authority.parent)) {
    throw ManagerApplyJournalIntegrityError(
        "bring-up proof candidate, original epochs or parent authority "
        "mismatch");
  }
  for (const auto &prior : previous) {
    if (prior.proof.result.status.has_value() &&
        prior.proof.result.status->apply_bring_up->parent_authority !=
            authority) {
      throw ManagerApplyJournalIntegrityError(
          "bring-up children do not share one exact parent claim/fence");
    }
  }
  if (proof.phase != ManagerApplyBringUpChildPhase::kAccepted) {
    const auto initial =
        std::find_if(previous.begin(), previous.end(), [&](const auto &value) {
          return value.proof.controllable_component_id ==
                     proof.controllable_component_id &&
                 value.proof.phase == ManagerApplyBringUpChildPhase::kAccepted;
        });
    if (initial == previous.end() ||
        !initial->proof.result.status.has_value() ||
        initial->proof.result.status->acceptance != status.acceptance ||
        initial->proof.result.status->apply_bring_up != status.apply_bring_up ||
        status.status_sequence <
            initial->proof.result.status->status_sequence) {
      throw ManagerApplyJournalIntegrityError(
          "bring-up child proof lacks its exact owner-minted acceptance");
    }
  }
  if (proof.phase == ManagerApplyBringUpChildPhase::kHolding) {
    if (status.status_owner_epoch != child->intent.request_context.request_owner_epoch ||
        status.phase !=
            core::ComponentLifecycleTransactionPhaseDTO::kVerifying ||
        status.terminal_result.has_value() || status.reconciliation_required ||
        status.error.has_value() ||
        status.requested_state.presence !=
            core::ComponentLifecycleRequestedStatePresenceDTO::kResolved ||
        !status.requested_state.state.has_value() ||
        status.current_state != status.requested_state.state) {
      throw ManagerApplyJournalIntegrityError(
          "bring-up holding is not a nonterminal verified configured target");
    }
    for (const auto &domain : status.domains) {
      const bool retired = !domain.expected_domain.has_value();
      if (domain.error.has_value() ||
          (retired ? domain.effect !=
                         core::ComponentLifecycleDomainEffectDTO::kKnownNoEffect
                   : (domain.action_state !=
                          core::ComponentLifecycleDomainActionStateDTO::
                              kSucceeded ||
                      !domain.current_scope.has_value() ||
                      !domain.resolved_provider_occurrence.has_value() ||
                      domain.requested_state.presence !=
                          core::ComponentLifecycleRequestedStatePresenceDTO::
                              kResolved ||
                      !domain.requested_state.state.has_value() ||
                      domain.current_state != domain.requested_state.state))) {
        throw ManagerApplyJournalIntegrityError(
            "bring-up holding lacks actual successful domain postconditions");
      }
    }
  }
  if (proof.phase == ManagerApplyBringUpChildPhase::kTerminal) {
    if (!status.terminal_result.has_value() &&
        status.phase !=
            core::ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown) {
      throw ManagerApplyJournalIntegrityError(
          "bring-up terminal proof is not a truthful terminal or unknown "
          "outcome");
    }
    if (BringUpProofSucceeded(proof)) {
      if (status.status_owner_epoch != child->intent.request_context.request_owner_epoch) {
        throw ManagerApplyJournalIntegrityError("a replaced child owner cannot manufacture successful live settlement");
      }
      const auto held = std::find_if(
          previous.begin(), previous.end(), [&](const auto &value) {
            return value.proof.controllable_component_id ==
                       proof.controllable_component_id &&
                   value.proof.phase == ManagerApplyBringUpChildPhase::kHolding;
          });
      if (held == previous.end() ||
          status.status_sequence < held->proof.result.status->status_sequence) {
        throw ManagerApplyJournalIntegrityError(
            "bring-up success lacks its original actual holding proof");
      }
    }
  }
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
        "; expected v19. Preserve this file, including interrupted operations; "
        "reconcile the original operations with their owner and retained "
        "evidence "
        "before explicitly selecting a fresh v19 journal. No migration, "
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

bool ManagerApplyBringUpChildRequestDTO::valid() const noexcept {
  return intent.locally_valid() &&
         (intent.intent == core::ComponentLifecycleIntentKindDTO::kPrepare ||
          intent.intent == core::ComponentLifecycleIntentKindDTO::kEnable ||
          intent.intent == core::ComponentLifecycleIntentKindDTO::kDisable) &&
         !intent.program_node_evidence.has_value() &&
         candidate_projection_revision.valid() &&
         core::IsExactLowercaseSha256Digest(
             candidate_retained_bundle_digest.value()) &&
         candidate_retained_bundle_bytes > 0U &&
         candidate_retained_bundle_bytes <=
             static_cast<std::uint64_t>(
                 std::numeric_limits<std::int64_t>::max()) &&
         !runtime_unit_id.empty() && !runtime_epoch.empty() &&
         !manager_runtime_epoch.empty();
}

bool ManagerApplyBringUpPlanDTO::valid() const noexcept {
  const auto *owner = std::get_if<NamedApplyPublicationTransactionDTO>(
      &expected_publication.transaction_ref);
  if (apply_id.empty() || !apply_generation.nonzero() ||
      !core::IsExactLowercaseSha256Digest(parent_intent_digest.value()) ||
      !expected_publication.valid() || owner == nullptr ||
      owner->apply_id != apply_id ||
      owner->apply_generation != apply_generation ||
      !core::IsExactLowercaseSha256Digest(
          predecessor_public_pointer_digest.value()) ||
      !core::IsExactLowercaseSha256Digest(
          expected_publication.private_materialization_content_digest
              .value()) ||
      expected_publication.expected_previous_publication_generation->value() >=
          static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) ||
      configured_components.empty() ||
      configured_components.size() > kMaximumManagerApplyFenceComponents ||
      children.empty() || children.size() > configured_components.size())
    return false;
  for (std::size_t index = 0U; index < configured_components.size(); ++index) {
    if (configured_components[index].empty() ||
        (index != 0U &&
         !(configured_components[index - 1U] < configured_components[index])))
      return false;
  }
  for (std::size_t index = 0U; index < children.size(); ++index) {
    const auto &child = children[index];
    if (!child.valid() ||
        child.candidate_projection_revision !=
            expected_publication.candidate_projection_revision ||
        !std::binary_search(configured_components.begin(),
                            configured_components.end(),
                            child.intent.controllable_component_id) ||
        (index != 0U &&
         !(children[index - 1U].intent.controllable_component_id <
           child.intent.controllable_component_id)) ||
        child.candidate_retained_bundle_digest !=
            children.front().candidate_retained_bundle_digest ||
        child.candidate_retained_bundle_bytes !=
            children.front().candidate_retained_bundle_bytes ||
        child.runtime_unit_id != children.front().runtime_unit_id ||
        child.runtime_epoch != children.front().runtime_epoch ||
        child.manager_runtime_epoch != children.front().manager_runtime_epoch)
      return false;
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (children[previous].intent.request_context.request_owner_epoch ==
              child.intent.request_context.request_owner_epoch &&
          children[previous].intent.request_context.request_id ==
              child.intent.request_context.request_id)
        return false;
    }
  }
  return true;
}

ContentDigest
ComputeManagerApplyIntentDigest(const ManagerApplyIntentDTO &intent) {
  return ContentDigest(EncodeIntent(intent).intent_digest);
}

ContentDigest ComputeManagerApplyPrivateMaterializationDigest(
    const ManagerApplyPrivateMaterializationDTO &materialization) {
  return ContentDigest(EncodePrivateMaterialization(materialization).digest);
}

ContentDigest
ComputeManagerApplyBringUpPlanDigest(const ManagerApplyBringUpPlanDTO &plan) {
  return ContentDigest(BringUpPlanDigest(EncodeBringUpPlan(plan)));
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
        static_cast<std::int64_t>(kMaximumManagerApplyIntents)) {
      throw ManagerApplyJournalLimitError(
          "Apply intent inventory reached its fixed limit");
    }

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
        "intent_digest,scene_initialization"
        ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
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
      VerifiedRecordMap<PersistedManagerApplyTerminalOutcome>,
      VerifiedRecordMap<PersistedManagerApplyBringUpPlan>,
      VerifiedRecordMap<std::vector<PersistedManagerApplyBringUpChildProof>>>;

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
        {apply_id, apply_generation, 0},
        [&] { return IntentUncachedLocked(apply_id, apply_generation); });
  }

  std::optional<PersistedManagerApplyBringUpPlan>
  BringUpPlanLocked(const ApplyId &apply_id, std::int64_t generation) const {
    return ReadVerifiedRecord<
        PersistedManagerApplyBringUpPlan>({apply_id, generation, 0}, [&] {
      Statement row(database_, "SELECT intent_digest,plan_encoding,plan_digest "
                               "FROM manager_apply_bring_up_plans WHERE "
                               "apply_id=? AND apply_generation=?");
      row.Bind(1, apply_id.value());
      row.Bind(2, generation);
      if (!row.Step())
        return std::optional<PersistedManagerApplyBringUpPlan>{};
      const auto intent_digest = row.Text(0);
      const auto encoding = row.Blob(1);
      const auto digest = row.Text(2);
      if (row.Step())
        throw ManagerApplyJournalIntegrityError("duplicate bring-up plan");
      try {
        auto plan = DecodeBringUpPlan(encoding);
        const auto intent = IntentLocked(apply_id, generation);
        if (!intent.has_value() || !plan.valid() || plan.apply_id != apply_id ||
            plan.apply_generation.value() !=
                static_cast<std::uint64_t>(generation) ||
            plan.parent_intent_digest != intent->intent_digest ||
            plan.parent_intent_digest.value() != intent_digest ||
            intent->intent.parent_authoring_claim.has_value() ||
            !intent->intent.expected_admission_generation.has_value() ||
            plan.expected_publication.candidate_retained_bundle_id !=
                intent->intent.candidate_retained_bundle_id ||
            plan.expected_publication.candidate_graph_revision !=
                intent->intent.candidate_graph_revision ||
            plan.expected_publication.candidate_projection_revision
                    .scene_revision !=
                intent->intent.expected_active_projection_revision
                    .scene_revision ||
            EncodeBringUpPlan(plan) != encoding ||
            BringUpPlanDigest(encoding) != digest) {
          throw ManagerApplyJournalIntegrityError(
              "stored bring-up plan is not bound to its exact parent");
        }
        const auto &child = plan.children.front();
        const ManagerApplyPrivateMaterializationDTO materialization{
            child.candidate_projection_revision.scene_revision.content_hash,
            apply_id,
            plan.apply_generation,
            child.candidate_projection_revision,
            child.candidate_retained_bundle_digest,
            child.candidate_retained_bundle_bytes};
        if (ComputeManagerApplyPrivateMaterializationDigest(materialization) !=
            plan.expected_publication.private_materialization_content_digest) {
          throw ManagerApplyJournalIntegrityError(
              "stored bring-up publication materialization mismatch");
        }
        return std::optional<PersistedManagerApplyBringUpPlan>{
            {std::move(plan), ContentDigest(digest)}};
      } catch (const ManagerApplyJournalIntegrityError &) {
        throw;
      } catch (const std::exception &) {
        throw ManagerApplyJournalIntegrityError(
            "stored bring-up plan contains an invalid value");
      }
    });
  }

  std::vector<PersistedManagerApplyBringUpChildProof>
  BringUpChildProofsLocked(const ApplyId &apply_id,
                           std::int64_t generation) const {
    const auto read = ReadVerifiedRecord<
        std::vector<PersistedManagerApplyBringUpChildProof>>(
        {apply_id, generation, 0}, [&] {
          std::vector<PersistedManagerApplyBringUpChildProof> proofs;
          const auto plan = BringUpPlanLocked(apply_id, generation);
          Statement rows(
              database_,
              "SELECT "
              "plan_digest,component_id,phase,result_encoding,proof_digest "
              "FROM manager_apply_bring_up_child_proofs WHERE apply_id=? AND "
              "apply_generation=? "
              "ORDER BY component_id,CASE phase WHEN 'accepted' THEN 0 WHEN "
              "'holding' THEN 1 ELSE 2 END");
          rows.Bind(1, apply_id.value());
          rows.Bind(2, generation);
          while (rows.Step()) {
            try {
              if (!plan.has_value() ||
                  proofs.size() >= plan->plan.children.size() * 3U) {
                throw ManagerApplyJournalIntegrityError(
                    "stored bring-up child inventory exceeds its exact plan");
              }
              const auto encoding = rows.Blob(3);
              ManagerApplyBringUpChildProofDTO proof{
                  apply_id,
                  ApplyGeneration(static_cast<std::uint64_t>(generation)),
                  ContentDigest(rows.Text(0)),
                  ControllableComponentId(rows.Text(1)),
                  ParseBringUpPhase(rows.Text(2)),
                  core::DecodeApplyDelegatedComponentLifecycleRequestResult(
                      encoding)};
              if (core::EncodeApplyDelegatedComponentLifecycleRequestResult(
                      proof.result) != encoding ||
                  BringUpProofDigest(proof, encoding) != rows.Text(4)) {
                throw ManagerApplyJournalIntegrityError(
                    "stored bring-up child proof digest mismatch");
              }
              ValidateBringUpProof(plan->plan, plan->plan_digest, proof,
                                   proofs);
              if (proof.result.status.has_value()) {
                const auto &authority =
                    proof.result.status->apply_bring_up->parent_authority;
                const auto parent = IntentLocked(apply_id, generation);
                const auto fence = FenceAcceptanceLocked(apply_id, generation);
                if (!parent.has_value() || !fence.has_value() ||
                    authority.parent_claim.intent.owner_epoch !=
                        parent->intent.request_context.request_owner_epoch ||
                    authority.parent_claim.intent.request_id !=
                        parent->intent.request_context.request_id ||
                    authority.parent_claim.intent
                            .expected_admission_generation !=
                        parent->intent.expected_admission_generation ||
                    authority.held_fence_query.configuration_fence_id !=
                        fence->accepted_fence.configuration_fence_id ||
                    authority.held_fence_query.configuration_fence_generation !=
                        fence->accepted_fence.configuration_fence_generation ||
                    authority.held_fence_query.expected_scope !=
                        fence->accepted_fence.captured_scope) {
                  throw ManagerApplyJournalIntegrityError(
                      "stored bring-up child is not joined to actual parent "
                      "fence/claim");
                }
              }
              if (BringUpProofSucceeded(proof))
                RequireBringUpPublicationLocked(plan->plan);
              proofs.push_back({std::move(proof), ContentDigest(rows.Text(4))});
            } catch (const ManagerApplyJournalIntegrityError &) {
              throw;
            } catch (const std::exception &) {
              throw ManagerApplyJournalIntegrityError(
                  "stored bring-up child proof contains an invalid value");
            }
          }
          return std::optional<
              std::vector<PersistedManagerApplyBringUpChildProof>>{
              std::move(proofs)};
        });
    return read.value_or(std::vector<PersistedManagerApplyBringUpChildProof>{});
  }

  void RequireBringUpPublicationLocked(
      const ManagerApplyBringUpPlanDTO &plan) const {
    // Avoid a recursive materialization -> child proofs -> publication chain:
    // verify the exact publication row with its existing digest encoder here.
    Statement row(
        database_,
        "SELECT candidate_retained_bundle_id,candidate_graph_revision,"
        "candidate_scene_id,candidate_scene_revision_id,candidate_scene_"
        "content_hash,"
        "expected_previous_publication_generation,private_materialization_"
        "content_digest,"
        "publication_generation,durable_public_pointer_content_digest,"
        "publication_receipt_digest "
        "FROM manager_apply_publication_receipts WHERE apply_id=? AND "
        "apply_generation=?");
    row.Bind(1, plan.apply_id.value());
    row.Bind(2,
             CheckedSigned(plan.apply_generation.value(), "apply_generation"));
    if (!row.Step())
      throw ManagerApplyJournalIntegrityError(
          "bring-up child success lacks exact actual publication");
    const auto &candidate =
        plan.expected_publication.candidate_projection_revision;
    const auto previous =
        CheckedSigned(plan.expected_publication
                          .expected_previous_publication_generation->value(),
                      "previous_publication_generation");
    if (row.Text(0) != candidate.retained_bundle_id.value() ||
        row.Text(1) != candidate.graph_revision.value() ||
        row.Text(2) != candidate.scene_revision.scene_id.value() ||
        row.Integer(3) != CheckedSigned(candidate.scene_revision.revision_id,
                                        "scene_revision") ||
        row.Text(4) != candidate.scene_revision.content_hash.value() ||
        row.Integer(5) != previous ||
        row.Text(6) != plan.expected_publication
                           .private_materialization_content_digest.value() ||
        row.Integer(7) != previous + 1) {
      throw ManagerApplyJournalIntegrityError(
          "bring-up child success has foreign publication identity");
    }
    const ManagerApplyPublicationReceiptDTO publication{
        plan.expected_publication.private_materialization_content_digest,
        {plan.expected_publication,
         ActiveBundlePublicationGeneration(
             static_cast<std::uint64_t>(row.Integer(7))),
         ContentDigest(row.Text(8))}};
    if (EncodePublicationReceipt(publication).digest != row.Text(9) ||
        row.Step()) {
      throw ManagerApplyJournalIntegrityError(
          "bring-up child success publication digest mismatch");
    }
  }

  void
  RequireBringUpProofSetLocked(const PersistedManagerApplyBringUpPlan &plan,
                               bool terminal) const {
    const auto proofs = BringUpChildProofsLocked(
        plan.plan.apply_id,
        CheckedSigned(plan.plan.apply_generation.value(), "apply_generation"));
    for (const auto &child : plan.plan.children) {
      const auto found =
          std::find_if(proofs.begin(), proofs.end(), [&](const auto &proof) {
            return proof.proof.controllable_component_id ==
                       child.intent.controllable_component_id &&
                   proof.proof.phase ==
                       (terminal ? ManagerApplyBringUpChildPhase::kTerminal
                                 : ManagerApplyBringUpChildPhase::kHolding);
          });
      if (found == proofs.end() ||
          (terminal && !BringUpProofSucceeded(found->proof))) {
        throw ManagerApplyJournalIntegrityError(
            "bring-up parent lacks every exact required child proof");
      }
    }
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
        BringUpPlanLocked(apply_id, apply_generation),
        BringUpChildProofsLocked(apply_id, apply_generation),
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
      const auto configuration_facts =
          DecodeExpectedConfigurations(active_configurations);
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
          configuration_facts.configurations,
          reason,
          configuration_facts.lineages,
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
      if (const auto plan = BringUpPlanLocked(apply_id, apply_generation);
          plan.has_value()) {
        if (plan->plan.expected_publication
                    .private_materialization_content_digest !=
                ContentDigest(stored_digest) ||
            plan->plan.expected_publication.candidate_projection_revision !=
                materialization.candidate_projection_revision ||
            plan->plan.children.front().candidate_retained_bundle_digest !=
                materialization.retained_bundle_canonical_content_digest ||
            plan->plan.children.front().candidate_retained_bundle_bytes !=
                materialization.retained_bundle_canonical_byte_size) {
          throw ManagerApplyJournalIntegrityError(
              "selected materialization is not joined to its exact original "
              "plan");
        }
        RequireBringUpProofSetLocked(*plan, false);
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
      if (const auto plan = BringUpPlanLocked(apply_id, apply_generation);
          plan.has_value() && plan->plan.expected_publication !=
                                  publication.publication_receipt.request) {
        throw ManagerApplyJournalIntegrityError(
            "selected publication differs from its exact original plan");
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
      if (const auto plan = BringUpPlanLocked(apply_id, apply_generation);
          plan.has_value()) {
        RequireBringUpProofSetLocked(*plan, true);
      }
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
                  "effect_disposition,outcome_state,terminal_outcome_digest "
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
        };
      }
      const auto encoded = EncodeTerminalOutcome(outcome);
      if (encoded.digest != stored_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored terminal Apply outcome digest is inconsistent");
      }
      if (outcome.state == ManagerApplyTerminalOutcomeState::kSucceeded) {
        if (const auto plan = BringUpPlanLocked(apply_id, apply_generation);
            plan.has_value()) {
          RequireBringUpProofSetLocked(*plan, true);
        }
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
    const auto bring_up_plan_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_bring_up_plans");
    const auto bring_up_child_proof_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_bring_up_child_proofs");
    const auto selected_materialization_count = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM manager_apply_private_materializations m "
        "JOIN manager_apply_bring_up_plans p ON p.apply_id=m.apply_id "
        "AND p.apply_generation=m.apply_generation");
    if (bring_up_plan_count < 0 || bring_up_plan_count > intent_count ||
        bring_up_child_proof_count < 0 || selected_materialization_count < 0 ||
        selected_materialization_count > bring_up_plan_count ||
        selected_materialization_count > private_materialization_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored bring-up cardinality is invalid");
    }
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
        private_materialization_count - selected_materialization_count >
            runtime_staged_ready_count ||
        attempt_count > affected_scope_count ||
        affected_scope_count > intent_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-fence journal cardinality is invalid");
    }
    Statement bring_up_plans(
        database_,
        "SELECT apply_id,apply_generation FROM manager_apply_bring_up_plans "
        "ORDER BY apply_id,apply_generation");
    std::int64_t observed_plans = 0;
    std::int64_t observed_child_proofs = 0;
    while (bring_up_plans.Step()) {
      const auto apply_id = ApplyId(bring_up_plans.Text(0));
      const auto generation = bring_up_plans.Integer(1);
      if (!BringUpPlanLocked(apply_id, generation).has_value() ||
          ++observed_plans > intent_count) {
        throw ManagerApplyJournalIntegrityError(
            "stored bring-up plan inventory is inconsistent");
      }
      observed_child_proofs += static_cast<std::int64_t>(
          BringUpChildProofsLocked(apply_id, generation).size());
    }
    if (observed_plans != bring_up_plan_count ||
        observed_child_proofs != bring_up_child_proof_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored bring-up child inventory has an unlinked or invalid proof");
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
        bring_up_plan_count,
        bring_up_child_proof_count,
    };
  }

  std::filesystem::path database_path_;
  CellId cell_id_;
  sqlite3 *database_{};
  mutable std::mutex mutex_;
  mutable VerifiedReads *verified_reads_{};
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto result = impl_->EnsureIntentValueLocked(intent);
  if (result.inserted)
    static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return result;
}

ManagerApplyBringUpIntentWriteResult
SqliteManagerApplyJournal::EnsureBringUpIntent(
    const ManagerApplyIntentDTO &intent,
    const ManagerApplyBringUpPlanDTO &plan) {
  const auto encoding = EncodeBringUpPlan(plan);
  const auto digest = BringUpPlanDigest(encoding);
  const auto encoded_intent = EncodeIntent(intent);
  if (plan.apply_id != intent.apply_id ||
      plan.apply_generation != intent.apply_generation ||
      plan.parent_intent_digest.value() != encoded_intent.intent_digest ||
      intent.parent_authoring_claim.has_value() ||
      !intent.expected_admission_generation.has_value()) {
    throw ManagerApplyJournalConflictError(
        "bring-up plan does not match its exact direct Manager parent");
  }
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto existing_plan = impl_->BringUpPlanLocked(
      intent.apply_id, encoded_intent.apply_generation);
  const auto existing_parent =
      impl_->IntentLocked(intent.apply_id, encoded_intent.apply_generation);
  if (existing_parent.has_value() && !existing_plan.has_value()) {
    throw ManagerApplyJournalConflictError(
        "an ordinary accepted Apply cannot acquire a bring-up plan");
  }
  if (existing_plan.has_value() &&
      (existing_plan->plan != plan ||
       existing_plan->plan_digest.value() != digest)) {
    throw ManagerApplyJournalConflictError(
        "Apply already has a different configured bring-up plan");
  }
  const auto parent = impl_->EnsureIntentValueLocked(intent);
  if (!existing_plan.has_value()) {
    Statement insert(impl_->database_,
                     "INSERT INTO manager_apply_bring_up_plans "
                     "(apply_id,apply_generation,intent_digest,plan_encoding,"
                     "plan_digest) VALUES (?,?,?,?,?)");
    insert.Bind(1, intent.apply_id.value());
    insert.Bind(2, encoded_intent.apply_generation);
    insert.Bind(3, encoded_intent.intent_digest);
    insert.BindBlob(4, encoding);
    insert.Bind(5, digest);
    static_cast<void>(insert.Step());
  }
  const auto stored = impl_->BringUpPlanLocked(intent.apply_id,
                                               encoded_intent.apply_generation);
  if (!stored.has_value() || stored->plan != plan ||
      stored->plan_digest.value() != digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted configured bring-up plan cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return {parent.record, *stored, parent.inserted};
}

std::optional<PersistedManagerApplyBringUpPlan>
SqliteManagerApplyJournal::ReadBringUpPlan(const ApplyId &apply_id,
                                           ApplyGeneration generation) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  const auto value = CheckedSigned(generation.value(), "apply_generation");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto result = impl_->BringUpPlanLocked(apply_id, value);
  transaction.Commit();
  return result;
}

std::optional<PersistedManagerApplyBringUpPlan>
SqliteManagerApplyJournal::ReadBringUpPlanByRequest(
    const RequestOwnerEpoch &owner, const RequestId &request) const {
  ValidateBoundedIdentity(owner.value(), "request_owner_epoch");
  ValidateBoundedIdentity(request.value(), "request_id");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto parent = impl_->RequestLocked(owner, request);
  const auto result =
      parent.has_value()
          ? impl_->BringUpPlanLocked(
                parent->intent.apply_id,
                CheckedSigned(parent->intent.apply_generation.value(),
                              "apply_generation"))
          : std::nullopt;
  transaction.Commit();
  return result;
}

ManagerApplyBringUpChildProofWriteResult
SqliteManagerApplyJournal::EnsureBringUpChildProof(
    const ManagerApplyBringUpChildProofDTO &proof) {
  const auto generation =
      CheckedSigned(proof.apply_generation.value(), "apply_generation");
  const auto encoding =
      core::EncodeApplyDelegatedComponentLifecycleRequestResult(proof.result);
  const auto digest = BringUpProofDigest(proof, encoding);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto plan = impl_->BringUpPlanLocked(proof.apply_id, generation);
  if (!plan.has_value())
    throw ManagerApplyJournalConflictError(
        "bring-up child has no exact original parent plan");
  const auto previous =
      impl_->BringUpChildProofsLocked(proof.apply_id, generation);
  ValidateBringUpProof(plan->plan, plan->plan_digest, proof, previous);
  for (const auto &existing : previous) {
    if (existing.proof.controllable_component_id ==
            proof.controllable_component_id &&
        existing.proof.phase == proof.phase) {
      if (existing.proof_digest.value() != digest ||
          core::EncodeApplyDelegatedComponentLifecycleRequestResult(
              existing.proof.result) != encoding) {
        throw ManagerApplyJournalConflictError(
            "bring-up child milestone conflicts with its original proof");
      }
      transaction.Commit();
      return {existing, false};
    }
  }
  Statement insert(impl_->database_,
                   "INSERT INTO manager_apply_bring_up_child_proofs "
                   "(apply_id,apply_generation,plan_digest,component_id,phase,"
                   "result_encoding,proof_digest) "
                   "VALUES (?,?,?,?,?,?,?)");
  insert.Bind(1, proof.apply_id.value());
  insert.Bind(2, generation);
  insert.Bind(3, proof.plan_digest.value());
  insert.Bind(4, proof.controllable_component_id.value());
  insert.Bind(5, BringUpPhaseName(proof.phase));
  insert.BindBlob(6, encoding);
  insert.Bind(7, digest);
  static_cast<void>(insert.Step());
  const auto stored =
      impl_->BringUpChildProofsLocked(proof.apply_id, generation);
  const auto result =
      std::find_if(stored.begin(), stored.end(), [&](const auto &value) {
        return value.proof.controllable_component_id ==
                   proof.controllable_component_id &&
               value.proof.phase == proof.phase;
      });
  if (result == stored.end() || result->proof_digest.value() != digest) {
    throw ManagerApplyJournalIntegrityError(
        "inserted bring-up child proof cannot be verified");
  }
  static_cast<void>(impl_->VerifyIntegrityLocked());
  transaction.Commit();
  return {*result, true};
}

std::optional<PersistedManagerApplyBringUpChildProof>
SqliteManagerApplyJournal::ReadBringUpChildProof(
    const ApplyId &apply_id, ApplyGeneration generation,
    const ControllableComponentId &component,
    ManagerApplyBringUpChildPhase phase) const {
  ValidateBoundedIdentity(apply_id.value(), "apply_id");
  ValidateBoundedIdentity(component.value(), "component_id");
  const auto value = CheckedSigned(generation.value(), "apply_generation");
  static_cast<void>(BringUpPhaseName(phase));
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
  const auto proofs = impl_->BringUpChildProofsLocked(apply_id, value);
  const auto found =
      std::find_if(proofs.begin(), proofs.end(), [&](const auto &proof) {
        return proof.proof.controllable_component_id == component &&
               proof.proof.phase == phase;
      });
  const auto result =
      found == proofs.end()
          ? std::optional<PersistedManagerApplyBringUpChildProof>{}
          : *found;
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
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
  if (const auto plan = impl_->BringUpPlanLocked(
          evidence.owner_transaction.apply_id, encoded.apply_generation);
      plan.has_value()) {
    impl_->RequireBringUpProofSetLocked(*plan, true);
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
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const bool succeeded =
      terminal_outcome.state == ManagerApplyTerminalOutcomeState::kSucceeded;
  if (succeeded) {
    if (const auto plan = impl_->BringUpPlanLocked(terminal_outcome.apply_id,
                                                   encoded.apply_generation);
        plan.has_value()) {
      impl_->RequireBringUpProofSetLocked(*plan, true);
    }
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
      "outcome_state,terminal_outcome_digest) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
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
  const auto verified_reads = impl_->UseVerifiedReadsLocked();
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
      claim.acquired_admission_generation !=
          claim.intent.expected_admission_generation ||
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
      status.bring_up_plan ? status.bring_up_plan->plan_digest
                           : status.intent.intent_digest,
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
        static_cast<void>(view.VerifyIntegrityLocked());
        for (const auto &claim : old_claims) {
          const auto intent = view.RequestLocked(claim.intent.owner_epoch,
                                                 claim.intent.request_id);
          if (!intent)
            throw ManagerApplyJournalIntegrityError(
                "configuration_apply_original_intent_missing");
          const auto status = view.StatusLocked(
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
        static_cast<void>(view.VerifyIntegrityLocked());
        const auto intent = view.RequestLocked(fresh_claim.intent.owner_epoch,
                                               fresh_claim.intent.request_id);
        if (!intent)
          throw ManagerApplyJournalIntegrityError(
              "fresh_apply_original_intent_missing");
        const auto status = view.StatusLocked(
            intent->intent.apply_id,
            CheckedSigned(intent->intent.apply_generation.value(),
                          "apply_generation"));
        if (!status)
          throw ManagerApplyJournalIntegrityError(
              "fresh_apply_original_status_missing");
        RequireExactManagerApplyClaim(fresh_claim, *status);
        if (status->bring_up_plan || !status->terminal_outcome ||
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
    Transaction snapshot(database, "BEGIN");
    ValidateApplyJournalIdentityAndSchema(database, cell_id, database_path);
    Statement row(
        database,
        "SELECT t.terminal_outcome_digest FROM manager_apply_intents i "
        "JOIN manager_apply_terminal_outcomes t "
        "ON t.apply_id=i.apply_id AND "
        "t.apply_generation=i.apply_generation "
        // A published success or a known refusal before any fence effect
        // settles this exact Manager claim. Later failures remain contained.
        "AND (t.outcome_state='succeeded' OR "
        "(t.outcome_state='failed' AND t.effect_disposition='known' "
        "AND t.failure_phase='accepted')) "
        "WHERE i.apply_id=? AND i.request_owner_epoch=? AND i.request_id=? "
        "AND i.intent_digest=? AND i.expected_admission_generation=? "
        "AND i.parent_authoring_claim IS NULL "
        "AND NOT EXISTS (SELECT 1 FROM manager_apply_bring_up_plans p "
        "WHERE p.apply_id=i.apply_id AND "
        "p.apply_generation=i.apply_generation)");
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

core::Result<PersistedManagerApplyStatus> ReadManagerApplyStatusReadOnly(
    const std::filesystem::path &database_path, const CellId &cell_id,
    const core::NamedApplyPublicationTransactionDTO &apply) {
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
    static_cast<void>(view.VerifyIntegrityLocked());
    auto status = view.StatusLocked(
        apply.apply_id,
        CheckedSigned(apply.apply_generation.value(), "apply_generation"));
    if (!status)
      return rejected();
    snapshot.Commit();
    return Reply::Ok(std::move(*status));
  } catch (const std::exception &) {
    if (raw)
      sqlite3_close(raw);
    return rejected();
  }
}

core::Result<PersistedManagerApplyStatus> ReadManagerApplyBringUpStatusReadOnly(
    const std::filesystem::path &database_path, const CellId &cell_id,
    const core::ApplyComponentBringUpReferenceDTO &parent) {
  const auto rejected = [](std::string reason) {
    return core::Result<PersistedManagerApplyStatus>::Err(core::ErrorDTO{
        "manager_apply_bring_up_read_unavailable",
        "the exact retained configured bring-up parent cannot be observed",
        {{"reason", std::move(reason)}}});
  };
  if (!parent.valid()) return rejected("parent_reference_invalid");
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
      static_cast<void>(view.VerifyIntegrityLocked());
      result = view.StatusLocked(parent.owner.apply_id,
          CheckedSigned(parent.owner.apply_generation.value(), "apply_generation"));
      if (!result || !result->bring_up_plan ||
          result->bring_up_plan->plan_digest != parent.plan_digest)
        throw ManagerApplyJournalIntegrityError("retained parent plan identity mismatch");
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

core::Result<PersistedManagerApplyStatus> ReadManagerApplyBringUpStatusReadOnly(
    const std::filesystem::path &database_path, const CellId &cell_id,
    const core::CellAdmissionClaimIntentDTO &parent_claim_intent) {
  const auto rejected = [](std::string reason) {
    return core::Result<PersistedManagerApplyStatus>::Err(core::ErrorDTO{
        "manager_apply_bring_up_read_unavailable",
        "the exact retained configured bring-up parent cannot be observed",
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
      static_cast<void>(view.VerifyIntegrityLocked());
      const auto intent = view.RequestLocked(parent_claim_intent.owner_epoch,
                                             parent_claim_intent.request_id);
      if (!intent ||
          intent->intent.apply_id.value() != parent_claim_intent.owner_transaction_id ||
          intent->intent.expected_admission_generation !=
              std::optional(parent_claim_intent.expected_admission_generation) ||
          intent->intent.parent_authoring_claim)
        throw ManagerApplyJournalIntegrityError("retained parent claim intent mismatch");
      result = view.StatusLocked(intent->intent.apply_id,
          CheckedSigned(intent->intent.apply_generation.value(), "apply_generation"));
      if (!result)
        throw ManagerApplyJournalIntegrityError("retained parent status missing");
      const core::CellAdmissionClaimIntentDTO expected{
          core::CellAdmissionClaimKindDTO::kManagerApply,
          core::CellAdmissionOwnerRoleDTO::kManager,
          intent->intent.request_context.request_owner_epoch,
          intent->intent.apply_id.value(),
          intent->intent.request_context.request_id,
          result->bring_up_plan ? result->bring_up_plan->plan_digest
                               : result->intent.intent_digest,
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
      static_cast<void>(view.VerifyIntegrityLocked());
      result = view.StatusLocked(apply.apply_id,
          CheckedSigned(apply.apply_generation.value(), "apply_generation"));
      if (!result)
        throw ManagerApplyJournalIntegrityError("ordinary configuration parent missing");
      if (result->bring_up_plan)
        throw ManagerApplyJournalIntegrityError("configuration parent has linked bring-up plan");
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
        detail == "configuration parent has linked bring-up plan" ||
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

core::Result<PersistedManagerApplyStatus> ReadNativeApplyParentStatusReadOnly(
    const std::filesystem::path &database_path, const CellId &cell_id,
    const core::NativeApplyPlanDTO &native_plan,
    const deployment::RetainedBundle &verified_candidate) {
  using ReadResult = core::Result<PersistedManagerApplyStatus>;
  const auto rejected = [](std::string reason) {
    return ReadResult::Err(core::ErrorDTO{
        "native_apply_parent_read_unavailable",
        "the exact retained native Apply parent cannot be observed",
        {{"reason", std::move(reason)}}});
  };
  sqlite3 *database = nullptr;
  try {
    if (!native_plan.apply.valid() ||
        !deployment::verify_retained_bundle_identity(verified_candidate))
      return rejected("native_plan_or_candidate_invalid");
    const auto &candidate = verified_candidate.value();
    const auto &graph = candidate.cell_graph;
    if (graph.cell_id != cell_id ||
        native_plan.candidate_retained_bundle_id !=
            verified_candidate.retained_bundle_id() ||
        native_plan.candidate_graph_revision != graph.graph_revision ||
        native_plan.components.size() > core::kMaximumCCLCComponentWatermarks ||
        native_plan.routes.size() >
            core::kMaximumCCLCActiveConfigurationWatermarks ||
        native_plan.removed_components.size() >
            core::kMaximumCCLCComponentWatermarks ||
        native_plan.removed_domains.size() >
            core::kMaximumCCLCActiveConfigurationWatermarks)
      return rejected("native_plan_candidate_mismatch");
    std::map<ControllableComponentId, std::vector<ControlDomainId>>
        component_domains;
    std::vector<ControllableComponentId> candidate_components;
    for (const auto &component : graph.controllable_components) {
      candidate_components.push_back(component.controllable_component_id);
      component_domains.emplace(component.controllable_component_id,
                                std::vector<ControlDomainId>{});
    }
    std::sort(candidate_components.begin(), candidate_components.end());
    for (const auto &binding : graph.control_bindings)
      component_domains.at(binding.controllable_component_id)
          .push_back(binding.control_domain_id);
    for (auto &[component, domains] : component_domains) {
      static_cast<void>(component);
      std::sort(domains.begin(), domains.end());
      domains.erase(std::unique(domains.begin(), domains.end()), domains.end());
    }
    std::vector<ControllableComponentId> planned_components;
    for (const auto &component : native_plan.components) {
      if (!component.generation.nonzero() ||
          component.generation.value() >
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max()) ||
          component.state < core::ComponentLifecycleStateDTO::kUndeclared ||
          component.state > core::ComponentLifecycleStateDTO::kDisabled)
        return rejected("native_component_fact_invalid");
      planned_components.push_back(component.component_id);
    }
    // Preserve the native journal's canonical ordering and detect duplicates;
    // these planned generations/states remain the captured CCLC owner's facts.
    if (planned_components != candidate_components)
      return rejected("native_component_coverage_mismatch");
    std::vector<ControlDomainId> candidate_domains;
    for (const auto &desired : candidate.desired_control_configurations)
      candidate_domains.push_back(desired.control_domain_id);
    std::sort(candidate_domains.begin(), candidate_domains.end());
    std::vector<ControlDomainId> planned_domains;
    for (const auto &route : native_plan.routes) {
      if (!route.generation.nonzero() ||
          route.generation.value() >
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max()))
        return rejected("native_route_fact_invalid");
      planned_domains.push_back(route.control_domain_id);
    }
    if (planned_domains != candidate_domains)
      return rejected("native_domain_coverage_mismatch");

    PreflightApplyJournal(database_path, cell_id);
    const auto opened = sqlite3_open_v2(
        database_path.string().c_str(), &database,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
        nullptr);
    if (opened != SQLITE_OK) {
      if (database != nullptr)
        sqlite3_close(database);
      database = nullptr;
      return rejected("parent_journal_unavailable");
    }
    Check(database, sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds),
          "configure read-only native parent timeout");
    Exec(database, "PRAGMA query_only=ON");
    std::optional<PersistedManagerApplyStatus> result;
    {
      Transaction snapshot(database, "BEGIN");
      ValidateApplyJournalIdentityAndSchema(database, cell_id, database_path);
      SqliteManagerApplyJournal::Impl view(database, cell_id);
      static_cast<void>(view.VerifyIntegrityLocked());
      auto status = view.StatusLocked(
          native_plan.apply.apply_id,
          CheckedSigned(native_plan.apply.apply_generation.value(),
                        "apply_generation"));
      if (!status || !status->affected_scope ||
          status->intent.intent.candidate_retained_bundle_id !=
              native_plan.candidate_retained_bundle_id ||
          status->intent.intent.candidate_graph_revision !=
              native_plan.candidate_graph_revision)
        throw ManagerApplyJournalIntegrityError(
            "native parent or affected scope identity mismatch");
      std::vector<ControllableComponentId> retained_components;
      std::vector<ControllableComponentId> removed_components;
      std::set<ControlDomainId> predecessor_domains;
      std::size_t candidate_rows = 0U;
      for (const auto &row :
           status->affected_scope->affected_scope.component_delta.components) {
        const auto identity = deployment::derive_component_runtime_identity(
            verified_candidate, row.controllable_component_id);
        const auto domains =
            component_domains.find(row.controllable_component_id);
        if (identity != row.candidate_runtime_identity ||
            (domains == component_domains.end()
                 ? !row.candidate_domains.empty()
                 : domains->second != row.candidate_domains))
          throw ManagerApplyJournalIntegrityError(
              "native candidate component identity mismatch");
        if (identity)
          ++candidate_rows;
        if (row.disposition == deployment::ApplyComponentDisposition::kRetain)
          retained_components.push_back(row.controllable_component_id);
        if (row.disposition == deployment::ApplyComponentDisposition::kRemove)
          removed_components.push_back(row.controllable_component_id);
        predecessor_domains.insert(row.predecessor_domains.begin(),
                                   row.predecessor_domains.end());
      }
      std::set<ControlDomainId> retained_domains;
      for (const auto &desired : candidate.desired_control_configurations)
        for (const auto &route : desired.binding_routes) {
          const auto binding = std::find_if(
              graph.control_bindings.begin(), graph.control_bindings.end(),
              [&](const auto &value) {
                return value.control_binding_id == route.control_binding_id;
              });
          if (binding == graph.control_bindings.end())
            throw ManagerApplyJournalIntegrityError(
                "native candidate desired binding missing");
          if (std::binary_search(retained_components.begin(),
                                 retained_components.end(),
                                 binding->controllable_component_id))
            retained_domains.insert(desired.control_domain_id);
        }
      std::vector<ControlDomainId> removed_domains;
      std::set_difference(predecessor_domains.begin(),
                          predecessor_domains.end(), candidate_domains.begin(),
                          candidate_domains.end(),
                          std::back_inserter(removed_domains));
      if (candidate_rows != candidate_components.size() ||
          native_plan.retained_components != retained_components ||
          native_plan.removed_components != removed_components ||
          native_plan.retained_domains !=
              std::vector<ControlDomainId>(retained_domains.begin(),
                                           retained_domains.end()) ||
          native_plan.removed_domains != removed_domains ||
          (status->bring_up_plan &&
           status->bring_up_plan->plan.configured_components !=
               candidate_components))
        throw ManagerApplyJournalIntegrityError(
            "native retained or removed closure mismatch");
      if (status->bring_up_plan) {
        const ContentDigest candidate_digest(
            RawDigest(verified_candidate.canonical_bytes()));
        for (const auto &child : status->bring_up_plan->plan.children)
          if (child.candidate_retained_bundle_digest != candidate_digest ||
              child.candidate_retained_bundle_bytes !=
                  verified_candidate.canonical_bytes().size())
            throw ManagerApplyJournalIntegrityError(
                "native child retained candidate bytes mismatch");
      }
      result = std::move(status);
      snapshot.Commit();
    }
    sqlite3_close(database);
    database = nullptr;
    return ReadResult::Ok(std::move(*result));
  } catch (const std::exception &) {
    if (database != nullptr)
      sqlite3_close(database);
    return rejected("parent_journal_or_native_identity_invalid");
  }
}

core::Result<std::optional<PersistedManagerApplyStatus>>
ReadNativeApplyPreparationFailureReadOnly(
    const std::filesystem::path &database_path, const CellId &cell_id,
    const core::NativeApplyPlanDTO &native_plan,
    const deployment::RetainedBundle &verified_candidate,
    const core::CellAdmissionClaimDTO &parent_claim,
    const core::CellAdmissionRecoveryReceiptQueryDTO &claim_query,
    const core::CellAdmissionRecoveryReceiptDTO &claim_receipt) {
  using ReadResult = core::Result<std::optional<PersistedManagerApplyStatus>>;
  const auto rejected = [](std::string reason) {
    return ReadResult::Err(
        core::ErrorDTO{"native_apply_preparation_failure_read_unavailable",
                       "the exact original native Apply preparation failure "
                       "cannot be observed",
                       {{"reason", std::move(reason)}}});
  };
  // These are actual supplied recovery values, never a Kernel callback or a
  // claim inferred from the owner journal. Validate them before entering SQL.
  if (!core::ValidateCellAdmissionRecoveryReceiptQuery(claim_query).ok() ||
      !core::ValidateCellAdmissionRecoveryReceipt(claim_receipt).ok() ||
      claim_query.intent != parent_claim.intent ||
      claim_query.expected_claim != std::optional(parent_claim) ||
      claim_query.expected_release ||
      claim_receipt.status !=
          core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
      claim_receipt.matched_claim != std::optional(parent_claim))
    return rejected("original_parent_not_held_exact");

  auto read = ReadNativeApplyParentStatusReadOnly(
      database_path, cell_id, native_plan, verified_candidate);
  if (!read.ok())
    return ReadResult::Err(read.error());
  auto status = std::move(read.value());
  const auto &intent = status.intent.intent;
  if (intent.parent_authoring_claim) {
    if (status.bring_up_plan || *intent.parent_authoring_claim != parent_claim)
      return rejected("native_authoring_parent_claim_mismatch");
  } else {
    if (!intent.expected_admission_generation)
      return rejected("native_parent_admission_identity_missing");
    const core::CellAdmissionClaimIntentDTO expected{
        core::CellAdmissionClaimKindDTO::kManagerApply,
        core::CellAdmissionOwnerRoleDTO::kManager,
        intent.request_context.request_owner_epoch,
        intent.apply_id.value(),
        intent.request_context.request_id,
        status.bring_up_plan ? status.bring_up_plan->plan_digest
                             : status.intent.intent_digest,
        *intent.expected_admission_generation,
        {},
        std::nullopt,
        std::nullopt,
        std::nullopt};
    if (parent_claim.intent != expected)
      return rejected("native_original_parent_claim_intent_mismatch");
  }
  for (const auto &proof : status.bring_up_child_proofs)
    if (!proof.proof.result.status ||
        !proof.proof.result.status->apply_bring_up ||
        proof.proof.result.status->apply_bring_up->parent_authority
                .parent_claim != parent_claim)
      return rejected("native_child_original_parent_claim_mismatch");

  const auto &terminal = status.terminal_outcome;
  const bool early =
      terminal &&
      terminal->terminal_outcome.state ==
          ManagerApplyTerminalOutcomeState::kUnknownEffect &&
      terminal->terminal_outcome.failure &&
      terminal->terminal_outcome.failure->effect_disposition ==
          ManagerApplyEffectDisposition::kUnknown &&
      terminal->terminal_outcome.failure->failure_code ==
          "runtime_composition_preparation_failed" &&
      terminal->terminal_outcome.failure->intent_digest ==
          status.intent.intent_digest &&
      terminal->terminal_outcome.failure->phase ==
          ManagerApplyPhase::kRuntimeConverged &&
      status.fence_attempt && status.fence_acceptance &&
      status.runtime_convergence_intent && status.latest_runtime_staged_ready &&
      !status.private_materialization && !status.publication_receipt &&
      !status.fence_release_intent && !status.fence_released &&
      std::all_of(status.bring_up_child_proofs.begin(),
                  status.bring_up_child_proofs.end(), [](const auto &proof) {
                    return proof.proof.phase ==
                           ManagerApplyBringUpChildPhase::kAccepted;
                  });
  if (!early)
    return ReadResult::Ok(std::nullopt);
  return ReadResult::Ok(std::move(status));
}
core::Result<void> ValidateManagerApplyBringUpCellAdmissionSettlementReadOnly(
    const std::filesystem::path &database_path, const CellId &cell_id,
    const core::CellAdmissionClaimDTO &claim,
    const core::CellAdmissionClaimReleaseDTO &release) {
  const auto rejected = [](std::string reason) {
    return core::Result<void>::Err(core::ErrorDTO{
        "invariant_violation",
        "Manager configured bring-up settlement is not owner-proven",
        {{"reason", std::move(reason)}}});
  };
  if (!core::ValidateCellAdmissionClaimIntent(claim.intent).ok() ||
      claim.intent.claim_kind !=
          core::CellAdmissionClaimKindDTO::kManagerApply ||
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
    return rejected("manager_bring_up_release_identity_mismatch");
  }
  sqlite3 *database = nullptr;
  const auto opened = sqlite3_open_v2(
      database_path.string().c_str(), &database,
      SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
      nullptr);
  if (opened != SQLITE_OK) {
    if (database != nullptr)
      sqlite3_close(database);
    return rejected("manager_bring_up_journal_unavailable");
  }
  try {
    Check(database, sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds),
          "configure read-only settlement timeout");
    Exec(database, "PRAGMA query_only=ON");
    {
      Transaction snapshot(database, "BEGIN");
      ValidateApplyJournalIdentityAndSchema(database, cell_id, database_path);
      SqliteManagerApplyJournal::Impl view(database, cell_id);
      static_cast<void>(view.VerifyIntegrityLocked());
      const auto parent =
          view.RequestLocked(claim.intent.owner_epoch, claim.intent.request_id);
      if (!parent.has_value() ||
          parent->intent.apply_id.value() !=
              claim.intent.owner_transaction_id ||
          parent->intent.expected_admission_generation !=
              claim.intent.expected_admission_generation ||
          parent->intent.parent_authoring_claim.has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "bring-up settlement parent identity mismatch");
      }
      const auto generation = CheckedSigned(
          parent->intent.apply_generation.value(), "apply_generation");
      const auto plan =
          view.BringUpPlanLocked(parent->intent.apply_id, generation);
      const auto terminal =
          view.TerminalOutcomeLocked(parent->intent.apply_id, generation);
      if (!plan.has_value() ||
          plan->plan_digest != claim.intent.request_payload_digest ||
          !terminal.has_value() ||
          terminal->terminal_outcome_digest !=
              release.settlement.evidence_digest) {
        throw ManagerApplyJournalIntegrityError(
            "bring-up settlement plan or terminal evidence mismatch");
      }
      const auto proofs =
          view.BringUpChildProofsLocked(parent->intent.apply_id, generation);
      if (terminal->terminal_outcome.succeeded()) {
        view.RequireBringUpProofSetLocked(*plan, true);
        // Every successful child observed the same full original claim, not
        // merely a matching attribution or a reconstructed owner epoch.
        for (const auto &proof : proofs) {
          if (proof.proof.result.status.has_value() &&
              proof.proof.result.status->apply_bring_up->parent_authority
                      .parent_claim != claim) {
            throw ManagerApplyJournalIntegrityError(
                "bring-up settlement is not the original held parent claim");
          }
        }
      } else if (terminal->terminal_outcome.state !=
                     ManagerApplyTerminalOutcomeState::kFailed ||
                 terminal->terminal_outcome.failure->phase !=
                     ManagerApplyPhase::kAccepted ||
                 terminal->terminal_outcome.failure->effect_disposition !=
                     ManagerApplyEffectDisposition::kKnown ||
                 view.FenceAttemptLocked(parent->intent.apply_id, generation)
                     .has_value() ||
                 !proofs.empty()) {
        throw ManagerApplyJournalIntegrityError(
            "effectful or unknown bring-up failure retains its exclusion");
      }
      snapshot.Commit();
    }
    sqlite3_close(database);
    database = nullptr;
    return core::Result<void>::Ok();
  } catch (const std::exception &) {
    sqlite3_close(database);
    return rejected("manager_bring_up_journal_read_failed");
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
