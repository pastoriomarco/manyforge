#include "manyforge_component_manager/manager_apply_journal.hpp"

#include "embedded_manager_apply_journal_schema.hpp"
#include <manyforge_persistence/sqlite_read_only_preflight.hpp>

#include <openssl/evp.h>
#include <sqlite3.h>

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
#include <utility>

namespace manyforge::component_manager {
namespace {

using core::ApplyGeneration;
using core::ApplyConfigurationFenceIntentDTO;
using core::ApplyId;
using core::ActiveControlConfigurationGeneration;
using core::ActiveControlConfigurationId;
using core::CellId;
using core::ConfigurationFenceAcceptedDTO;
using core::ConfigurationFenceBindingScopeDTO;
using core::ConfigurationFenceDomainScopeDTO;
using core::ConfigurationFenceGeneration;
using core::ConfigurationFenceId;
using core::ConfigurationFenceScopeDTO;
using core::ControllableComponentId;
using core::ControlBindingId;
using core::ControlBindingRevision;
using core::ControlDomainId;
using core::ControlDomainRevision;
using core::ContentDigest;
using core::DomainActiveControlConfigurationDTO;
using core::GraphRevision;
using core::ProjectionRevisionDTO;
using core::PublicRequestContextDTO;
using core::RequestId;
using core::RequestOwnerEpoch;
using core::RetainedBundleId;
using core::ResourceId;
using core::SceneId;
using core::SceneRevision;

constexpr std::string_view kDigestDomain =
    "manyforge.component-manager-apply-intent.v1";
constexpr std::string_view kFenceAttemptDigestDomain =
    "manyforge.component-manager-apply-fence-attempt.v1";
constexpr std::string_view kFenceAcceptanceDigestDomain =
    "manyforge.component-manager-apply-fence-acceptance.v1";
constexpr int kBusyTimeoutMilliseconds = 10000;
constexpr std::int64_t kWalCheckpointTargetBytes =
    16LL * 1024LL * 1024LL;
constexpr std::int64_t kExpectedSchemaObjectCardinality = 12;

[[noreturn]] void ThrowSqlite(
    sqlite3* database,
    const int result,
    const std::string_view context) {
  const std::string detail =
      database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
  if (result == SQLITE_FULL || result == SQLITE_TOOBIG) {
    throw ManagerApplyJournalLimitError(
        std::string(context) + ": SQLite storage ceiling exhausted: " +
        detail);
  }
  if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
    throw ManagerApplyJournalBusyError(
        std::string(context) + ": " + detail);
  }
  if (result == SQLITE_CONSTRAINT) {
    throw ManagerApplyJournalConflictError(
        std::string(context) + ": " + detail);
  }
  throw ManagerApplyJournalError(
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
            statement_,
            index,
            value.data(),
            static_cast<int>(value.size()),
            SQLITE_TRANSIENT),
        "bind SQLite text");
  }

  void Bind(const int index, const std::int64_t value) {
    Check(
        database_,
        sqlite3_bind_int64(statement_, index, value),
        "bind SQLite integer");
  }

  void BindNull(const int index) {
    Check(
        database_,
        sqlite3_bind_null(statement_, index),
        "bind SQLite null");
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
    const auto* text = sqlite3_column_text(statement_, column);
    if (text == nullptr) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-journal text column is null");
    }
    return {
        reinterpret_cast<const char*>(text),
        static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)),
    };
  }

  [[nodiscard]] std::optional<std::string> OptionalText(
      const int column) const {
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

SchemaObjects ReadSchemaObjects(sqlite3* database) {
  Statement statement(
      database,
      "SELECT type,name,sql FROM sqlite_master "
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
        ":memory:",
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        nullptr);
    if (result != SQLITE_OK) {
      const std::string detail =
          database == nullptr ? sqlite3_errstr(result) :
                                sqlite3_errmsg(database);
      if (database != nullptr) {
        sqlite3_close(database);
      }
      throw ManagerApplyJournalError(
          "open in-memory Apply schema validator: " + detail);
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

void ValidateBoundedIdentity(
    const std::string_view value,
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

void ValidateBoundedText(
    const std::string_view value,
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
      std::all_of(
          value.begin() + 7,
          value.end(),
          [](const char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
          });
}

void ValidateDigest(
    const std::string_view value,
    const std::string_view label) {
  if (!IsSha256Digest(value)) {
    throw std::invalid_argument(
        std::string(label) + " must be a lowercase sha256 digest");
  }
}

std::int64_t CheckedSigned(
    const std::uint64_t value,
    const std::string_view label) {
  if (value == 0U ||
      value >
          static_cast<std::uint64_t>(
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
  auto* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    throw ManagerApplyJournalIntegrityError(
        "cannot allocate SHA-256 context");
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

void AppendField(
    std::string& record,
    const std::string_view name,
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
  std::string intent_digest;
};

EncodedIntent EncodeIntent(const ManagerApplyIntentDTO& intent) {
  if (!intent.valid()) {
    throw std::invalid_argument("manager Apply intent is invalid");
  }
  ValidateBoundedIdentity(
      intent.request_context.request_id.value(), "request_id");
  ValidateBoundedIdentity(
      intent.request_context.request_owner_epoch.value(),
      "request_owner_epoch");
  ValidateBoundedText(
      intent.request_context.source_label,
      kMaximumManagerApplySourceLabelBytes,
      "source_label");
  if (intent.request_context.correlation_id.has_value()) {
    ValidateBoundedText(
        *intent.request_context.correlation_id,
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
  ValidateBoundedIdentity(
      intent.expected_active_projection_revision.scene_revision.scene_id
          .value(),
      "predecessor_scene_id");
  ValidateDigest(
      intent.expected_active_projection_revision.scene_revision.content_hash
          .value(),
      "predecessor_scene_content_hash");
  ValidateBoundedIdentity(
      intent.candidate_retained_bundle_id.value(),
      "candidate_retained_bundle_id");
  ValidateBoundedIdentity(
      intent.candidate_graph_revision.value(),
      "candidate_graph_revision");

  const auto generation =
      CheckedSigned(intent.apply_generation.value(), "apply_generation");
  const auto scene_revision = CheckedSigned(
      intent.expected_active_projection_revision.scene_revision.revision_id,
      "predecessor_scene_revision_id");

  std::string record;
  AppendField(record, "domain", kDigestDomain);
  AppendField(record, "request_id", intent.request_context.request_id.value());
  AppendField(
      record,
      "request_owner_epoch",
      intent.request_context.request_owner_epoch.value());
  AppendField(
      record, "source_label", intent.request_context.source_label);
  AppendField(
      record,
      "correlation_id_present",
      intent.request_context.correlation_id.has_value() ? "1" : "0");
  AppendField(
      record,
      "correlation_id",
      intent.request_context.correlation_id.value_or(""));
  AppendField(record, "apply_id", intent.apply_id.value());
  AppendField(
      record, "apply_generation", std::to_string(generation));
  AppendField(record, "cell_id", intent.cell_id.value());
  AppendField(
      record,
      "predecessor_retained_bundle_id",
      intent.expected_active_projection_revision.retained_bundle_id.value());
  AppendField(
      record,
      "predecessor_graph_revision",
      intent.expected_active_projection_revision.graph_revision.value());
  AppendField(
      record,
      "predecessor_scene_id",
      intent.expected_active_projection_revision.scene_revision.scene_id
          .value());
  AppendField(
      record,
      "predecessor_scene_revision_id",
      std::to_string(scene_revision));
  AppendField(
      record,
      "predecessor_scene_content_hash",
      intent.expected_active_projection_revision.scene_revision.content_hash
          .value());
  AppendField(
      record,
      "candidate_retained_bundle_id",
      intent.candidate_retained_bundle_id.value());
  AppendField(
      record,
      "candidate_graph_revision",
      intent.candidate_graph_revision.value());
  return {generation, scene_revision, RawDigest(record)};
}

std::int64_t CheckedNonnegativeSigned(
    const std::uint64_t value,
    const std::string_view label) {
  if (value > static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument(
        std::string(label) + " must fit the nonnegative SQLite int64 range");
  }
  return static_cast<std::int64_t>(value);
}

std::string EncodeTokens(const std::vector<std::string>& tokens) {
  std::string result = std::to_string(tokens.size()) + ";";
  for (const auto& token : tokens) {
    result += std::to_string(token.size());
    result.push_back(':');
    result += token;
  }
  return result;
}

std::uint64_t ParseCanonicalUnsigned(
    const std::string_view value,
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

std::vector<std::string> DecodeTokens(
    const std::string_view encoding,
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
    throw ManagerApplyJournalIntegrityError(
        std::string("stored ") + std::string(label) +
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
    const auto size_value = ParseCanonicalUnsigned(
        encoding.substr(cursor, colon - cursor), label);
    if (size_value > encoding.size() - colon - 1U) {
      throw ManagerApplyJournalIntegrityError(
          std::string("stored ") + std::string(label) +
          " token exceeds its encoding");
    }
    const auto size = static_cast<std::size_t>(size_value);
    result.emplace_back(encoding.substr(colon + 1U, size));
    cursor = colon + 1U + size;
  }
  if (cursor != encoding.size() || EncodeTokens(result) != encoding) {
    throw ManagerApplyJournalIntegrityError(
        std::string("stored ") + std::string(label) +
        " encoding is noncanonical");
  }
  return result;
}

std::string EncodeDomains(
    const std::vector<ConfigurationFenceDomainScopeDTO>& domains) {
  std::vector<std::string> tokens;
  tokens.reserve(domains.size() * 2U);
  for (const auto& domain : domains) {
    ValidateBoundedIdentity(domain.control_domain_id.value(),
                            "control_domain_id");
    ValidateBoundedIdentity(domain.control_domain_revision.value(),
                            "control_domain_revision");
    tokens.push_back(domain.control_domain_id.value());
    tokens.push_back(domain.control_domain_revision.value());
  }
  return EncodeTokens(tokens);
}

std::string EncodeBindings(
    const std::vector<ConfigurationFenceBindingScopeDTO>& bindings) {
  std::vector<std::string> tokens;
  tokens.reserve(bindings.size() * 2U);
  for (const auto& binding : bindings) {
    ValidateBoundedIdentity(binding.control_binding_id.value(),
                            "control_binding_id");
    ValidateBoundedIdentity(binding.control_binding_revision.value(),
                            "control_binding_revision");
    tokens.push_back(binding.control_binding_id.value());
    tokens.push_back(binding.control_binding_revision.value());
  }
  return EncodeTokens(tokens);
}

std::string EncodeResources(const std::vector<ResourceId>& resources) {
  std::vector<std::string> tokens;
  tokens.reserve(resources.size());
  for (const auto& resource : resources) {
    ValidateBoundedIdentity(resource.value(), "resource_id");
    tokens.push_back(resource.value());
  }
  return EncodeTokens(tokens);
}

std::string EncodeActiveConfigurations(
    const std::vector<DomainActiveControlConfigurationDTO>& configurations) {
  std::vector<std::string> tokens;
  tokens.reserve(configurations.size() * 3U);
  for (const auto& configuration : configurations) {
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

ConfigurationFenceScopeDTO DecodeScope(
    const std::string& component,
    const std::string& domains_encoding,
    const std::string& bindings_encoding,
    const std::string& resources_encoding) {
  ValidateBoundedIdentity(component, "stored controllable_component_id");
  const auto domain_tokens = DecodeTokens(
      domains_encoding, kMaximumManagerApplyFenceDomains * 2U, "domains");
  const auto binding_tokens = DecodeTokens(
      bindings_encoding,
      kMaximumManagerApplyFenceBindings * 2U,
      "bindings");
  const auto resource_tokens = DecodeTokens(
      resources_encoding, kMaximumManagerApplyFenceResources, "resources");
  if (domain_tokens.size() % 2U != 0U ||
      binding_tokens.size() % 2U != 0U) {
    throw ManagerApplyJournalIntegrityError(
        "stored fence scope tuple cardinality is invalid");
  }
  std::vector<ConfigurationFenceDomainScopeDTO> decoded_domains;
  std::vector<ConfigurationFenceBindingScopeDTO> decoded_bindings;
  std::vector<ResourceId> decoded_resources;
  for (std::size_t index = 0U; index < domain_tokens.size(); index += 2U) {
    decoded_domains.push_back({
        ControlDomainId(domain_tokens[index]),
        ControlDomainRevision(domain_tokens[index + 1U]),
    });
  }
  for (std::size_t index = 0U; index < binding_tokens.size(); index += 2U) {
    decoded_bindings.push_back({
        ControlBindingId(binding_tokens[index]),
        ControlBindingRevision(binding_tokens[index + 1U]),
    });
  }
  for (const auto& resource : resource_tokens) {
    decoded_resources.emplace_back(resource);
  }
  ConfigurationFenceScopeDTO scope{
      ControllableComponentId(component),
      std::move(decoded_domains),
      std::move(decoded_bindings),
      std::move(decoded_resources),
  };
  if (!scope.valid()) {
    throw ManagerApplyJournalIntegrityError("stored fence scope is invalid");
  }
  return scope;
}

std::vector<DomainActiveControlConfigurationDTO>
DecodeActiveConfigurations(const std::string& encoding) {
  const auto tokens = DecodeTokens(
      encoding,
      kMaximumManagerApplyFenceDomains * 3U,
      "active configurations");
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

bool SameFenceIntent(
    const ApplyConfigurationFenceIntentDTO& lhs,
    const ApplyConfigurationFenceIntentDTO& rhs) {
  return lhs.owner_request_context == rhs.owner_request_context &&
      lhs.owner_transaction == rhs.owner_transaction &&
      lhs.scope == rhs.scope &&
      lhs.expected_retained_bundle_id == rhs.expected_retained_bundle_id &&
      lhs.expected_graph_revision == rhs.expected_graph_revision &&
      lhs.expected_operation_resource_generation ==
          rhs.expected_operation_resource_generation &&
      lhs.expected_fence_generation == rhs.expected_fence_generation &&
      lhs.expected_active_control_configurations ==
          rhs.expected_active_control_configurations &&
      lhs.reason == rhs.reason;
}

bool SameAcceptedFence(
    const ConfigurationFenceAcceptedDTO& lhs,
    const ConfigurationFenceAcceptedDTO& rhs) {
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
  std::string domains;
  std::string bindings;
  std::string resources;
  std::string active_configurations;
  std::string digest;
};

EncodedFenceAttempt EncodeFenceAttempt(
    const ContentDigest& intent_digest,
    const ApplyConfigurationFenceIntentDTO& intent) {
  if (intent.scope.domains.size() > kMaximumManagerApplyFenceDomains ||
      intent.scope.bindings.size() > kMaximumManagerApplyFenceBindings ||
      intent.scope.resources.size() > kMaximumManagerApplyFenceResources) {
    throw ManagerApplyJournalLimitError(
        "Apply configuration-fence scope exceeds a fixed cardinality limit");
  }
  if (!intent.valid()) {
    throw std::invalid_argument("Apply configuration-fence intent is invalid");
  }
  ValidateDigest(intent_digest.value(), "intent_digest");
  ValidateBoundedIdentity(intent.owner_request_context.request_id.value(),
                          "request_id");
  ValidateBoundedIdentity(
      intent.owner_request_context.request_owner_epoch.value(),
      "request_owner_epoch");
  ValidateBoundedText(intent.owner_request_context.source_label,
                      kMaximumManagerApplySourceLabelBytes,
                      "source_label");
  if (intent.owner_request_context.correlation_id.has_value()) {
    ValidateBoundedText(*intent.owner_request_context.correlation_id,
                        kMaximumManagerApplyCorrelationIdBytes,
                        "correlation_id");
  }
  ValidateBoundedIdentity(intent.owner_transaction.apply_id.value(),
                          "apply_id");
  ValidateBoundedIdentity(intent.scope.controllable_component_id.value(),
                          "controllable_component_id");
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
      EncodeDomains(intent.scope.domains),
      EncodeBindings(intent.scope.bindings),
      EncodeResources(intent.scope.resources),
      EncodeActiveConfigurations(
          intent.expected_active_control_configurations),
      {},
  };
  std::string record;
  AppendField(record, "domain", kFenceAttemptDigestDomain);
  AppendField(record, "intent_digest", intent_digest.value());
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
  AppendField(record, "controllable_component_id",
              intent.scope.controllable_component_id.value());
  AppendField(record, "domains_encoding", encoded.domains);
  AppendField(record, "bindings_encoding", encoded.bindings);
  AppendField(record, "resources_encoding", encoded.resources);
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
  std::string domains;
  std::string bindings;
  std::string resources;
  std::string replay_status;
  std::string digest;
};

EncodedFenceAcceptance EncodeFenceAcceptance(
    const ApplyId& apply_id,
    const ApplyGeneration apply_generation,
    const ContentDigest& fence_attempt_digest,
    const ConfigurationFenceAcceptedDTO& accepted) {
  if (accepted.captured_scope.domains.size() >
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
  AppendField(record, "captured_component_id",
              accepted.captured_scope.controllable_component_id.value());
  AppendField(record, "captured_domains_encoding", encoded.domains);
  AppendField(record, "captured_bindings_encoding", encoded.bindings);
  AppendField(record, "captured_resources_encoding", encoded.resources);
  AppendField(record, "accepted_at_ns", std::to_string(accepted.accepted_at_ns));
  AppendField(record, "replay_status", encoded.replay_status);
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
        size >
            static_cast<std::uintmax_t>(
                kMaximumManagerApplyJournalDatabasePageImageBytes)) {
      throw ManagerApplyJournalLimitError(
          "Apply-journal database file exceeds its ceiling");
    }
  } else if (
      error &&
      error != std::make_error_code(std::errc::no_such_file_or_directory)) {
    throw std::invalid_argument(
        "cannot inspect manager Apply-journal database path");
  }

  error.clear();
  const auto parent =
      path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
  if (!std::filesystem::exists(parent, error) ||
      !std::filesystem::is_directory(parent, error)) {
    throw std::invalid_argument(
        "Apply-journal database parent must exist");
  }
  return path;
}

void ValidateApplyJournalMetadata(
    sqlite3* database,
    const CellId& cell_id) {
  Statement row(database, "SELECT * FROM apply_journal_metadata");
  if (!row.Step()) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal metadata singleton is missing");
  }
  if (row.Integer(0) != 1 ||
      row.Text(1) != kManagerApplyJournalSchemaId ||
      row.Integer(2) != kManagerApplyJournalSchemaVersion ||
      row.Text(3) !=
          RawDigest(detail::kEmbeddedManagerApplyJournalSchema)) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal metadata mismatch");
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
          static_cast<std::int64_t>(kMaximumManagerApplyFenceDomains) ||
      row.Integer(8) !=
          static_cast<std::int64_t>(kMaximumManagerApplyFenceBindings) ||
      row.Integer(9) !=
          static_cast<std::int64_t>(kMaximumManagerApplyFenceResources) ||
      row.Integer(10) !=
          kMaximumManagerApplyJournalDatabasePageImageBytes) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal resource-limit metadata mismatch");
  }
  if (row.Step()) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal metadata singleton is invalid");
  }
}

void ValidateApplyJournalBeforeConfiguration(
    sqlite3* database,
    const CellId& cell_id) {
  const auto existing = ReadSchemaObjects(database);
  const auto application_id =
      ScalarInteger(database, "PRAGMA application_id");
  const auto user_version =
      ScalarInteger(database, "PRAGMA user_version");
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

void PreflightApplyJournal(
    const std::filesystem::path& database_path,
    const CellId& cell_id) {
  try {
    static_cast<void>(persistence::PreflightExistingSqliteReadOnly(
        database_path,
        {
            static_cast<std::uintmax_t>(
                kMaximumManagerApplyJournalDatabasePageImageBytes),
            static_cast<std::uintmax_t>(
                kMaximumManagerApplyJournalDatabasePageImageBytes),
        },
        [&cell_id](sqlite3* database) {
          ValidateApplyJournalBeforeConfiguration(database, cell_id);
        }));
  } catch (const persistence::SqliteReadOnlyPreflightError& error) {
    throw ManagerApplyJournalIntegrityError(
        "Apply-journal read-only preflight failed: " +
        std::string(error.what()));
  }
}

}  // namespace

bool ManagerApplyIntentDTO::valid() const noexcept {
  return !request_context.request_id.empty() &&
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
    PreflightApplyJournal(database_path_, cell_id_);
    const int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX |
        SQLITE_OPEN_NOFOLLOW;
    const auto result = sqlite3_open_v2(
        database_path_.string().c_str(), &database_, flags, nullptr);
    if (result != SQLITE_OK) {
      const std::string error =
          database_ == nullptr ? sqlite3_errstr(result) :
                                 sqlite3_errmsg(database_);
      if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
      }
      throw ManagerApplyJournalError(
          "cannot open Apply-journal database: " + error);
    }
    try {
      Check(
          database_,
          sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds),
          "configure SQLite busy timeout");
      ValidateApplyJournalBeforeConfiguration(database_, cell_id_);
      Configure();
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
      throw ManagerApplyJournalClosedError(
          "manager Apply journal is closed");
    }
  }

  void Close() {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    sqlite3_wal_checkpoint_v2(
        database_,
        nullptr,
        SQLITE_CHECKPOINT_TRUNCATE,
        nullptr,
        nullptr);
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
    Check(
        database_,
        sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds),
        "configure SQLite busy timeout");
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kBusyTimeoutMilliseconds);
    while (true) {
      try {
        Statement journal_mode(database_, "PRAGMA journal_mode = WAL");
        if (!journal_mode.Step() || journal_mode.Text(0) != "wal") {
          throw ManagerApplyJournalIntegrityError(
              "SQLite WAL journal mode is required");
        }
        break;
      } catch (const ManagerApplyJournalBusyError&) {
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
    const auto journal_size_limit = ScalarInteger(
        database_,
        "PRAGMA journal_size_limit = " +
            std::to_string(kWalCheckpointTargetBytes));
    if (journal_size_limit != kWalCheckpointTargetBytes) {
      throw ManagerApplyJournalIntegrityError(
          "SQLite WAL journal-size policy is required");
    }
    const auto page_size = ScalarInteger(database_, "PRAGMA page_size");
    if (page_size <= 0) {
      throw ManagerApplyJournalIntegrityError(
          "SQLite page size is invalid");
    }
    const auto maximum_pages = std::max<std::int64_t>(
        1,
        kMaximumManagerApplyJournalDatabasePageImageBytes / page_size);
    const auto page_count = ScalarInteger(database_, "PRAGMA page_count");
    if (page_count < 0 || page_count > maximum_pages) {
      throw ManagerApplyJournalLimitError(
          "SQLite main database page image exceeds its ceiling");
    }
    const auto effective_maximum_pages = ScalarInteger(
        database_,
        "PRAGMA max_page_count = " + std::to_string(maximum_pages));
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
          "maximum_fence_domains,maximum_fence_bindings,"
          "maximum_fence_resources,maximum_main_database_page_image_bytes"
          ") VALUES (1,?,?,?,?,?,?,?,?,?,?)");
      metadata.Bind(1, kManagerApplyJournalSchemaId);
      metadata.Bind(2, kManagerApplyJournalSchemaVersion);
      metadata.Bind(
          3, RawDigest(detail::kEmbeddedManagerApplyJournalSchema));
      metadata.Bind(4, cell_id_.value());
      metadata.Bind(
          5, static_cast<std::int64_t>(kMaximumManagerApplyIntents));
      metadata.Bind(
          6, static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts));
      metadata.Bind(
          7, static_cast<std::int64_t>(kMaximumManagerApplyFenceDomains));
      metadata.Bind(
          8, static_cast<std::int64_t>(kMaximumManagerApplyFenceBindings));
      metadata.Bind(
          9, static_cast<std::int64_t>(kMaximumManagerApplyFenceResources));
      metadata.Bind(
          10, kMaximumManagerApplyJournalDatabasePageImageBytes);
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

  std::optional<std::int64_t> GenerationHighWaterLocked(
      const ApplyId& apply_id) const {
    Statement row(
        database_,
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

  std::optional<PersistedManagerApplyIntent> IntentLocked(
      const ApplyId& apply_id,
      const std::int64_t apply_generation) const {
    Statement row(
        database_,
        "SELECT apply_id,apply_generation,request_id,request_owner_epoch,"
        "source_label,correlation_id,predecessor_retained_bundle_id,"
        "predecessor_graph_revision,predecessor_scene_id,"
        "predecessor_scene_revision_id,predecessor_scene_content_hash,"
        "candidate_retained_bundle_id,candidate_graph_revision,intent_digest "
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
    const auto stored_digest = row.Text(13);
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
                static_cast<std::uint64_t>(
                    predecessor_scene_revision_id),
                ContentDigest(predecessor_scene_content_hash),
            },
        },
        RetainedBundleId(candidate_retained_bundle_id),
        GraphRevision(candidate_graph_revision),
    };
    EncodedIntent encoded;
    try {
      encoded = EncodeIntent(intent);
    } catch (const std::exception&) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply intent is invalid");
    }
    if (encoded.intent_digest != stored_digest) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply intent digest is inconsistent");
    }
    return PersistedManagerApplyIntent{
        std::move(intent), ContentDigest(stored_digest)};
  }

  std::optional<PersistedManagerApplyIntent> RequestLocked(
      const RequestOwnerEpoch& request_owner_epoch,
      const RequestId& request_id) const {
    Statement row(
        database_,
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

  std::optional<PersistedManagerApplyFenceAttempt> FenceAttemptLocked(
      const ApplyId& apply_id,
      const std::int64_t apply_generation) const {
    Statement row(
        database_,
        "SELECT intent_digest,request_id,request_owner_epoch,source_label,"
        "correlation_id,controllable_component_id,domains_encoding,"
        "bindings_encoding,resources_encoding,expected_retained_bundle_id,"
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
    const auto request_id = row.Text(1);
    const auto owner_epoch = row.Text(2);
    const auto source_label = row.Text(3);
    const auto correlation_id = row.OptionalText(4);
    const auto component = row.Text(5);
    const auto domains = row.Text(6);
    const auto bindings = row.Text(7);
    const auto resources = row.Text(8);
    const auto retained_bundle = row.Text(9);
    const auto graph_revision = row.Text(10);
    const auto operation_generation = row.Integer(11);
    const auto fence_generation = row.Integer(12);
    const auto active_configurations = row.Text(13);
    const auto reason = row.Text(14);
    const auto stored_digest = row.Text(15);
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
          DecodeScope(component, domains, bindings, resources),
          RetainedBundleId(retained_bundle),
          GraphRevision(graph_revision),
          static_cast<std::uint64_t>(operation_generation),
          ConfigurationFenceGeneration(
              static_cast<std::uint64_t>(fence_generation)),
          DecodeActiveConfigurations(active_configurations),
          reason,
      };
      const ContentDigest parent_digest(intent_digest);
      const auto encoded = EncodeFenceAttempt(parent_digest, intent);
      if (encoded.digest != stored_digest) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-fence attempt digest is inconsistent");
      }
      const auto parent = IntentLocked(apply_id, apply_generation);
      if (!parent.has_value() || parent->intent_digest != parent_digest ||
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
          ContentDigest(stored_digest),
      };
    } catch (const ManagerApplyJournalIntegrityError&) {
      throw;
    } catch (const std::exception&) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-fence attempt contains an invalid value");
    }
  }

  std::optional<PersistedManagerApplyFenceAcceptance> FenceAcceptanceLocked(
      const ApplyId& apply_id,
      const std::int64_t apply_generation) const {
    Statement row(
        database_,
        "SELECT fence_attempt_digest,configuration_fence_id,"
        "configuration_fence_generation,captured_component_id,"
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
    const auto component = row.Text(3);
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
          DecodeScope(component, domains, bindings, resources),
          accepted_at_ns,
          status,
      };
      const ContentDigest parent_digest(attempt_digest);
      const auto encoded = EncodeFenceAcceptance(
          apply_id,
          ApplyGeneration(static_cast<std::uint64_t>(apply_generation)),
          parent_digest,
          accepted);
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
    } catch (const ManagerApplyJournalIntegrityError&) {
      throw;
    } catch (const std::exception&) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-fence acceptance contains an invalid value");
    }
  }

  ManagerApplyJournalIntegrityReport VerifyIntegrityLocked() const {
    Statement integrity(database_, "PRAGMA integrity_check");
    if (!integrity.Step() || integrity.Text(0) != "ok" ||
        integrity.Step()) {
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
    const auto intent_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_intents");
    const auto distinct_apply_ids = ScalarInteger(
        database_,
        "SELECT COUNT(*) FROM ("
        "SELECT apply_id FROM manager_apply_intents GROUP BY apply_id)");
    if (intent_count < 0 ||
        intent_count >
            static_cast<std::int64_t>(kMaximumManagerApplyIntents) ||
        distinct_apply_ids < 0 || distinct_apply_ids > intent_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-journal cardinality is invalid");
    }

    Statement rows(
        database_,
        "SELECT apply_id,apply_generation,request_owner_epoch,request_id "
        "FROM manager_apply_intents "
        "ORDER BY apply_id,apply_generation LIMIT ?");
    rows.Bind(
        1, static_cast<std::int64_t>(kMaximumManagerApplyIntents + 1U));
    std::map<ApplyId, std::int64_t> expected_generations;
    std::set<std::pair<RequestOwnerEpoch, RequestId>> request_identities;
    std::int64_t observed = 0;
    while (rows.Step()) {
      ++observed;
      if (observed > static_cast<std::int64_t>(
                         kMaximumManagerApplyIntents)) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply intent inventory exceeds its ceiling");
      }
      const auto apply_id = ApplyId(rows.Text(0));
      const auto generation = rows.Integer(1);
      const auto request_owner_epoch = RequestOwnerEpoch(rows.Text(2));
      const auto request_id = RequestId(rows.Text(3));
      auto [expected, inserted] =
          expected_generations.emplace(apply_id, 1);
      if (generation != expected->second) {
        throw ManagerApplyJournalIntegrityError(
            "Apply generations are not contiguous for one Apply identity");
      }
      ++expected->second;
      if (!request_identities
               .emplace(request_owner_epoch, request_id)
               .second) {
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
        request_identities.size() !=
            static_cast<std::size_t>(intent_count)) {
      throw ManagerApplyJournalIntegrityError(
          "Apply intent inventory is inconsistent");
    }
    const auto attempt_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_fence_attempts");
    const auto acceptance_count = ScalarInteger(
        database_, "SELECT COUNT(*) FROM manager_apply_fence_acceptances");
    if (attempt_count < 0 || acceptance_count < 0 ||
        attempt_count > static_cast<std::int64_t>(
                            kMaximumManagerApplyFenceAttempts) ||
        acceptance_count > attempt_count || attempt_count > intent_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-fence journal cardinality is invalid");
    }
    Statement attempts(
        database_,
        "SELECT apply_id,apply_generation FROM manager_apply_fence_attempts "
        "ORDER BY apply_id,apply_generation LIMIT ?");
    attempts.Bind(
        1,
        static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts + 1U));
    std::int64_t observed_attempts = 0;
    while (attempts.Step()) {
      ++observed_attempts;
      if (observed_attempts > static_cast<std::int64_t>(
                                  kMaximumManagerApplyFenceAttempts) ||
          !FenceAttemptLocked(
               ApplyId(attempts.Text(0)), attempts.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-fence attempt inventory is inconsistent");
      }
    }
    Statement acceptances(
        database_,
        "SELECT apply_id,apply_generation "
        "FROM manager_apply_fence_acceptances "
        "ORDER BY apply_id,apply_generation LIMIT ?");
    acceptances.Bind(
        1,
        static_cast<std::int64_t>(kMaximumManagerApplyFenceAttempts + 1U));
    std::int64_t observed_acceptances = 0;
    while (acceptances.Step()) {
      ++observed_acceptances;
      if (observed_acceptances > static_cast<std::int64_t>(
                                     kMaximumManagerApplyFenceAttempts) ||
          !FenceAcceptanceLocked(
               ApplyId(acceptances.Text(0)), acceptances.Integer(1))
               .has_value()) {
        throw ManagerApplyJournalIntegrityError(
            "stored Apply-fence acceptance inventory is inconsistent");
      }
    }
    if (observed_attempts != attempt_count ||
        observed_acceptances != acceptance_count) {
      throw ManagerApplyJournalIntegrityError(
          "stored Apply-fence inventory count is inconsistent");
    }
    return {
        cell_id_,
        std::string(kManagerApplyJournalSchemaId),
        kManagerApplyJournalSchemaVersion,
        RawDigest(detail::kEmbeddedManagerApplyJournalSchema),
        intent_count,
        distinct_apply_ids,
        static_cast<std::int64_t>(request_identities.size()),
        attempt_count,
        acceptance_count,
    };
  }

  std::filesystem::path database_path_;
  CellId cell_id_;
  sqlite3* database_{};
  mutable std::mutex mutex_;
  bool closed_{};
};

SqliteManagerApplyJournal::SqliteManagerApplyJournal(
    std::filesystem::path database_path,
    CellId cell_id)
    : impl_(std::make_unique<Impl>(
          std::move(database_path), std::move(cell_id))) {}

SqliteManagerApplyJournal::~SqliteManagerApplyJournal() = default;

SqliteManagerApplyJournal::SqliteManagerApplyJournal(
    SqliteManagerApplyJournal&&) noexcept = default;

SqliteManagerApplyJournal& SqliteManagerApplyJournal::operator=(
    SqliteManagerApplyJournal&&) noexcept = default;

const std::filesystem::path&
SqliteManagerApplyJournal::database_path() const noexcept {
  return impl_->database_path_;
}

const CellId& SqliteManagerApplyJournal::cell_id() const noexcept {
  return impl_->cell_id_;
}

ManagerApplyIntentWriteResult SqliteManagerApplyJournal::EnsureIntent(
    const ManagerApplyIntentDTO& intent) {
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
  if (const auto existing_request = impl_->RequestLocked(
          intent.request_context.request_owner_epoch,
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
  const auto expected_generation =
      high_water.has_value() ? *high_water + 1 : 1;
  if (encoded.apply_generation != expected_generation) {
    throw ManagerApplyJournalConflictError(
        "Apply generation must advance its durable high-water mark by "
        "exactly one");
  }
  const auto intent_count = ScalarInteger(
      impl_->database_, "SELECT COUNT(*) FROM manager_apply_intents");
  if (intent_count >=
      static_cast<std::int64_t>(kMaximumManagerApplyIntents)) {
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
      "candidate_retained_bundle_id,candidate_graph_revision,intent_digest"
      ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
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
  insert.Bind(
      9,
      intent.expected_active_projection_revision.scene_revision.scene_id
          .value());
  insert.Bind(10, encoded.predecessor_scene_revision_id);
  insert.Bind(
      11,
      intent.expected_active_projection_revision.scene_revision.content_hash
          .value());
  insert.Bind(12, intent.candidate_retained_bundle_id.value());
  insert.Bind(13, intent.candidate_graph_revision.value());
  insert.Bind(14, encoded.intent_digest);
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
    const ApplyId& apply_id,
    const ApplyGeneration apply_generation) const {
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

std::optional<PersistedManagerApplyIntent>
SqliteManagerApplyJournal::ReadRequest(
    const RequestOwnerEpoch& request_owner_epoch,
    const RequestId& request_id) const {
  ValidateBoundedIdentity(
      request_owner_epoch.value(), "request_owner_epoch");
  ValidateBoundedIdentity(request_id.value(), "request_id");
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto result =
      impl_->RequestLocked(request_owner_epoch, request_id);
  transaction.Commit();
  return result;
}

std::optional<ApplyGeneration>
SqliteManagerApplyJournal::ReadApplyGenerationHighWater(
    const ApplyId& apply_id) const {
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

ManagerApplyFenceAttemptWriteResult
SqliteManagerApplyJournal::EnsureFenceAttempt(
    const ContentDigest& intent_digest,
    const ApplyConfigurationFenceIntentDTO& fence_intent) {
  const auto encoded = EncodeFenceAttempt(intent_digest, fence_intent);
  const auto& apply_id = fence_intent.owner_transaction.apply_id;
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto parent = impl_->IntentLocked(apply_id, encoded.apply_generation);
  if (!parent.has_value() || parent->intent_digest != intent_digest) {
    throw ManagerApplyJournalConflictError(
        "Apply-fence attempt does not name its exact durable intent");
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
  if (attempt_count >= static_cast<std::int64_t>(
                           kMaximumManagerApplyFenceAttempts)) {
    throw ManagerApplyJournalLimitError(
        "Apply-fence attempt inventory reached its fixed limit");
  }
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_fence_attempts ("
      "apply_id,apply_generation,intent_digest,request_id,"
      "request_owner_epoch,source_label,correlation_id,"
      "controllable_component_id,domains_encoding,bindings_encoding,"
      "resources_encoding,expected_retained_bundle_id,"
      "expected_graph_revision,expected_operation_resource_generation,"
      "expected_fence_generation,active_configurations_encoding,reason,"
      "fence_attempt_digest) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, intent_digest.value());
  insert.Bind(4, fence_intent.owner_request_context.request_id.value());
  insert.Bind(
      5, fence_intent.owner_request_context.request_owner_epoch.value());
  insert.Bind(6, fence_intent.owner_request_context.source_label);
  if (fence_intent.owner_request_context.correlation_id.has_value()) {
    insert.Bind(7, *fence_intent.owner_request_context.correlation_id);
  } else {
    insert.BindNull(7);
  }
  insert.Bind(8, fence_intent.scope.controllable_component_id.value());
  insert.Bind(9, encoded.domains);
  insert.Bind(10, encoded.bindings);
  insert.Bind(11, encoded.resources);
  insert.Bind(12, fence_intent.expected_retained_bundle_id.value());
  insert.Bind(13, fence_intent.expected_graph_revision.value());
  insert.Bind(14, encoded.operation_generation);
  insert.Bind(15, encoded.fence_generation);
  insert.Bind(16, encoded.active_configurations);
  insert.Bind(17, fence_intent.reason);
  insert.Bind(18, encoded.digest);
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
    const ApplyId& apply_id,
    const ApplyGeneration apply_generation) const {
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
    const ApplyId& apply_id,
    const ApplyGeneration apply_generation,
    const ContentDigest& fence_attempt_digest,
    const ConfigurationFenceAcceptedDTO& accepted_fence) {
  const auto encoded = EncodeFenceAcceptance(
      apply_id, apply_generation, fence_attempt_digest, accepted_fence);
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN IMMEDIATE");
  static_cast<void>(impl_->VerifyIntegrityLocked());
  const auto attempt = impl_->FenceAttemptLocked(
      apply_id, encoded.apply_generation);
  if (!attempt.has_value() ||
      attempt->fence_attempt_digest != fence_attempt_digest) {
    throw ManagerApplyJournalConflictError(
        "Apply-fence acceptance does not name its exact durable attempt");
  }
  if (!accepted_fence.matches(attempt->fence_intent)) {
    throw ManagerApplyJournalConflictError(
        "Apply-fence acceptance does not match its exact attempt");
  }
  if (const auto existing = impl_->FenceAcceptanceLocked(
          apply_id, encoded.apply_generation);
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
  if (acceptance_count >= static_cast<std::int64_t>(
                              kMaximumManagerApplyFenceAttempts)) {
    throw ManagerApplyJournalLimitError(
        "Apply-fence acceptance inventory reached its fixed limit");
  }
  Statement insert(
      impl_->database_,
      "INSERT INTO manager_apply_fence_acceptances ("
      "apply_id,apply_generation,fence_attempt_digest,"
      "configuration_fence_id,configuration_fence_generation,"
      "captured_component_id,captured_domains_encoding,"
      "captured_bindings_encoding,captured_resources_encoding,"
      "accepted_at_ns,replay_status,acceptance_digest) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.Bind(1, apply_id.value());
  insert.Bind(2, encoded.apply_generation);
  insert.Bind(3, fence_attempt_digest.value());
  insert.Bind(4, accepted_fence.configuration_fence_id.value());
  insert.Bind(5, encoded.fence_generation);
  insert.Bind(6, accepted_fence.captured_scope.controllable_component_id.value());
  insert.Bind(7, encoded.domains);
  insert.Bind(8, encoded.bindings);
  insert.Bind(9, encoded.resources);
  insert.Bind(10, accepted_fence.accepted_at_ns);
  insert.Bind(11, encoded.replay_status);
  insert.Bind(12, encoded.digest);
  static_cast<void>(insert.Step());
  const auto stored = impl_->FenceAcceptanceLocked(
      apply_id, encoded.apply_generation);
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
    const ApplyId& apply_id,
    const ApplyGeneration apply_generation) const {
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

ManagerApplyJournalIntegrityReport
SqliteManagerApplyJournal::VerifyIntegrity() const {
  std::lock_guard lock(impl_->mutex_);
  impl_->RequireOpen();
  Transaction transaction(impl_->database_, "BEGIN");
  const auto report = impl_->VerifyIntegrityLocked();
  transaction.Commit();
  return report;
}

void SqliteManagerApplyJournal::Close() {
  impl_->Close();
}

}  // namespace manyforge::component_manager
