#include "managed_kernel_apply_fence_owner.hpp"

#include "kernel_apply_fence_rpc_internal.hpp"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/route_fence_record_codec_internal.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_persistence/sqlite_read_only_preflight.hpp>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace manyforge::roboplan_backend {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;
namespace persistence = manyforge::persistence;

constexpr std::string_view kSchemaId =
    "manyforge.internal.kernel-configuration-fence-ledger.v6";
constexpr int kSchemaVersion = 6;
constexpr int kSqliteApplicationId = 1296451142;
constexpr std::uint64_t kMaximumFenceRecords = 256U;
constexpr std::uint64_t kMaximumDatabasePageImageBytes = 64U * 1024U * 1024U;

[[noreturn]] void Fail(std::string detail) {
  throw std::runtime_error("Managed Kernel Apply-fence owner: " +
                           std::move(detail));
}

[[noreturn]] void FailSqlite(sqlite3 *database, const std::string_view action) {
  const auto code =
      database == nullptr ? SQLITE_ERROR : sqlite3_extended_errcode(database);
  const auto *message = database == nullptr ? "SQLite database is absent"
                                            : sqlite3_errmsg(database);
  Fail(std::string(action) + " failed [" + std::to_string(code) +
       "]: " + message);
}

void Require(const bool condition, std::string detail) {
  if (!condition) {
    Fail(std::move(detail));
  }
}

template <typename Value, typename Operation>
[[nodiscard]] core::Result<Value>
InvokeRouteFencePort(Operation &&operation) noexcept {
  try {
    return core::Result<Value>::Ok(operation());
  } catch (const std::exception &error) {
    return core::Result<Value>::Err(
        core::ErrorDTO{"route_configuration_fence_rejected", error.what(), {}});
  } catch (...) {
    return core::Result<Value>::Err(
        core::ErrorDTO{"route_configuration_fence_rejected",
                       "route-configuration fence operation failed",
                       {}});
  }
}

[[nodiscard]] std::optional<int>
DescriptorFd(const std::filesystem::path &path) {
  constexpr std::string_view prefix = "/proc/self/fd/";
  const auto text = path.generic_string();
  if (!text.starts_with(prefix) || text.size() == prefix.size() ||
      (text[prefix.size()] == '0' && text.size() != prefix.size() + 1U)) {
    return std::nullopt;
  }
  unsigned int descriptor = 0U;
  const auto parsed = std::from_chars(text.data() + prefix.size(),
                                      text.data() + text.size(), descriptor);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      descriptor == 0U || descriptor > static_cast<unsigned int>(INT_MAX)) {
    return std::nullopt;
  }
  return static_cast<int>(descriptor);
}

[[nodiscard]] std::filesystem::path
ExistingPlainDirectory(std::filesystem::path path,
                       const std::string_view label) {
  Require(!path.empty(), std::string(label) + " is empty");
  constexpr std::string_view proc_fd_prefix = "/proc/self/fd/";
  if (path.generic_string().starts_with(proc_fd_prefix)) {
    const auto descriptor = DescriptorFd(path);
    Require(descriptor.has_value(),
            std::string(label) + " is not an exact retained descriptor path");
    struct stat status {};
    Require(::fstat(*descriptor, &status) == 0 && S_ISDIR(status.st_mode),
            std::string(label) +
                " is not an existing retained descriptor directory");
    return path;
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  Require(!error && !std::filesystem::is_symlink(status) &&
              std::filesystem::is_directory(status),
          std::string(label) + " is not an existing non-symlink directory");
  auto absolute = std::filesystem::absolute(path, error);
  Require(!error, std::string(label) + " cannot be resolved");
  return absolute.lexically_normal();
}

void RequireDisjointDirectories(const std::filesystem::path &left,
                                const std::filesystem::path &right,
                                const std::string_view label) {
  std::error_code error;
  const bool equivalent = std::filesystem::equivalent(left, right, error);
  Require(!error, std::string(label) + " cannot be compared");
  Require(!equivalent,
          std::string(label) + " must refer to distinct directories");
}

[[nodiscard]] std::filesystem::path
ValidateLedgerPath(std::filesystem::path path) {
  Require(!path.empty() && path.has_filename(), "ledger path is empty");
  const auto parent =
      ExistingPlainDirectory(path.parent_path(), "ledger parent directory");
  path = parent / path.filename();
  struct stat status {};
  if (::lstat(path.c_str(), &status) == 0) {
    Require(S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode),
            "ledger path is not a regular file");
  } else {
    Require(errno == ENOENT, "ledger path cannot be inspected");
  }
  return path;
}

class FileLock final {
public:
  explicit FileLock(const std::filesystem::path &ledger_path) {
    const auto lock_path = ledger_path.string() + ".lock";
    descriptor_ =
        ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
               S_IRUSR | S_IWUSR);
    if (descriptor_ < 0) {
      Fail("cannot open the durable-owner lock: " +
           std::string(std::strerror(errno)));
    }
    struct stat status {};
    if (::fstat(descriptor_, &status) != 0 || !S_ISREG(status.st_mode)) {
      const auto detail = std::string(std::strerror(errno));
      ::close(descriptor_);
      descriptor_ = -1;
      Fail("durable-owner lock is not a regular file: " + detail);
    }
    if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
      const auto detail = std::string(std::strerror(errno));
      ::close(descriptor_);
      descriptor_ = -1;
      Fail("another process owns the Kernel Apply-fence ledger: " + detail);
    }
  }

  ~FileLock() {
    if (descriptor_ >= 0) {
      static_cast<void>(::flock(descriptor_, LOCK_UN));
      static_cast<void>(::close(descriptor_));
    }
  }

  FileLock(const FileLock &) = delete;
  FileLock &operator=(const FileLock &) = delete;

private:
  int descriptor_{-1};
};

class Statement final {
public:
  Statement(sqlite3 *database, const std::string_view sql)
      : database_(database) {
    if (sqlite3_prepare_v2(database_, std::string(sql).c_str(), -1, &statement_,
                           nullptr) != SQLITE_OK) {
      FailSqlite(database_, "prepare statement");
    }
  }

  ~Statement() {
    if (statement_ != nullptr) {
      static_cast<void>(sqlite3_finalize(statement_));
    }
  }

  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;

  void Text(const int index, const std::string_view value) {
    if (sqlite3_bind_text(statement_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      FailSqlite(database_, "bind text");
    }
  }

  void Blob(const int index, const std::string_view value) {
    if (sqlite3_bind_blob(statement_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      FailSqlite(database_, "bind blob");
    }
  }

  void Integer(const int index, const std::uint64_t value) {
    Require(value <= static_cast<std::uint64_t>(
                         std::numeric_limits<sqlite3_int64>::max()),
            "generation exceeds the durable integer domain");
    if (sqlite3_bind_int64(statement_, index,
                           static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
      FailSqlite(database_, "bind integer");
    }
  }

  [[nodiscard]] int Step() {
    const auto result = sqlite3_step(statement_);
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
      FailSqlite(database_, "step statement");
    }
    return result;
  }

  [[nodiscard]] std::string ColumnText(const int index) const {
    Require(sqlite3_column_type(statement_, index) == SQLITE_TEXT,
            "durable text column has the wrong type");
    const auto *value = sqlite3_column_text(statement_, index);
    const auto size = sqlite3_column_bytes(statement_, index);
    Require(value != nullptr && size >= 0, "durable text column is absent");
    return {reinterpret_cast<const char *>(value),
            static_cast<std::size_t>(size)};
  }

  [[nodiscard]] std::string ColumnBlob(const int index) const {
    Require(sqlite3_column_type(statement_, index) == SQLITE_BLOB,
            "durable blob column has the wrong type");
    const auto *value = sqlite3_column_blob(statement_, index);
    const auto size = sqlite3_column_bytes(statement_, index);
    Require(value != nullptr && size > 0, "durable blob column is absent");
    return {static_cast<const char *>(value), static_cast<std::size_t>(size)};
  }

  [[nodiscard]] std::optional<std::string>
  OptionalColumnBlob(const int index) const {
    if (sqlite3_column_type(statement_, index) == SQLITE_NULL) {
      return std::nullopt;
    }
    return ColumnBlob(index);
  }

  [[nodiscard]] std::uint64_t ColumnInteger(const int index) const {
    Require(sqlite3_column_type(statement_, index) == SQLITE_INTEGER,
            "durable integer column has the wrong type");
    const auto value = sqlite3_column_int64(statement_, index);
    Require(value >= 0, "durable generation is negative");
    return static_cast<std::uint64_t>(value);
  }

private:
  sqlite3 *database_;
  sqlite3_stmt *statement_{};
};

void Exec(sqlite3 *database, const std::string_view sql) {
  char *message = nullptr;
  if (sqlite3_exec(database, std::string(sql).c_str(), nullptr, nullptr,
                   &message) != SQLITE_OK) {
    const std::string detail =
        message == nullptr ? sqlite3_errmsg(database) : message;
    sqlite3_free(message);
    Fail("SQLite command failed: " + detail);
  }
}

[[nodiscard]] std::uint64_t ReadSingleInteger(sqlite3 *database,
                                              const std::string_view sql,
                                              const std::string_view label) {
  Statement statement(database, sql);
  Require(statement.Step() == SQLITE_ROW, std::string(label) + " is absent");
  const auto value = statement.ColumnInteger(0);
  Require(statement.Step() == SQLITE_DONE,
          std::string(label) + " has extra rows");
  return value;
}

[[nodiscard]] bool ValidateLedgerEnvelope(sqlite3 *database) {
  const auto application_id = ReadSingleInteger(
      database, "PRAGMA application_id", "durable ledger application ID");
  const auto user_version = ReadSingleInteger(database, "PRAGMA user_version",
                                              "durable ledger schema version");
  const auto schema_object_count = ReadSingleInteger(
      database,
      "SELECT COUNT(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'",
      "durable ledger schema-object count");
  const bool fresh =
      application_id == 0U && user_version == 0U && schema_object_count == 0U;
  if (fresh) {
    return true;
  }
  Require(user_version == static_cast<std::uint64_t>(kSchemaVersion),
          "fence ledger version " + std::to_string(user_version) +
              " differs from required version " +
              std::to_string(kSchemaVersion) +
              "; preserve the file and obtain explicit maintainer direction to "
              "reconcile any retained fences before initializing new execution "
              "state; no migration, conversion or deletion was performed");
  Require(application_id == static_cast<std::uint64_t>(kSqliteApplicationId) &&
              schema_object_count == 2U,
          "existing durable ledger has an unknown schema envelope");
  Statement objects(database,
                    "SELECT name,type FROM sqlite_schema WHERE name NOT LIKE "
                    "'sqlite_%' ORDER BY name");
  Require(objects.Step() == SQLITE_ROW &&
              objects.ColumnText(0) == "kernel_apply_fence_meta" &&
              objects.ColumnText(1) == "table" &&
              objects.Step() == SQLITE_ROW &&
              objects.ColumnText(0) == "kernel_apply_fences" &&
              objects.ColumnText(1) == "table" && objects.Step() == SQLITE_DONE,
          "existing durable ledger has foreign schema objects");
  return false;
}

void ValidateExistingLedgerForPreflight(sqlite3 *database,
                                        const core::CellId &cell_id) {
  const bool fresh = ValidateLedgerEnvelope(database);
  Statement integrity(database, "PRAGMA quick_check");
  Require(integrity.Step() == SQLITE_ROW && integrity.ColumnText(0) == "ok" &&
              integrity.Step() == SQLITE_DONE,
          "durable ledger failed its SQLite integrity check");
  if (fresh) {
    return;
  }
  Statement metadata(database, "SELECT schema_id,schema_version,cell_id,"
                               "maximum_fence_records,"
                               "maximum_database_page_image_bytes FROM "
                               "kernel_apply_fence_meta WHERE id=1");
  Require(metadata.Step() == SQLITE_ROW &&
              metadata.ColumnText(0) == kSchemaId &&
              metadata.ColumnInteger(1) == kSchemaVersion &&
              metadata.ColumnText(2) == cell_id.value() &&
              metadata.ColumnInteger(3) == kMaximumFenceRecords &&
              metadata.ColumnInteger(4) == kMaximumDatabasePageImageBytes &&
              metadata.Step() == SQLITE_DONE,
          "durable ledger metadata differs from this owner");
  Require(
      ReadSingleInteger(database, "SELECT COUNT(*) FROM kernel_apply_fences",
                        "durable fence record count") <= kMaximumFenceRecords,
      "durable ledger exceeds its fixed record bound");
}

class Transaction final {
public:
  explicit Transaction(sqlite3 *database) : database_(database) {
    Exec(database_, "BEGIN IMMEDIATE");
  }

  ~Transaction() {
    if (!committed_) {
      static_cast<void>(
          sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr));
    }
  }

  void Commit() {
    Exec(database_, "COMMIT");
    committed_ = true;
  }

  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

private:
  sqlite3 *database_;
  bool committed_{};
};

void AppendField(std::string &bytes, const std::string_view value) {
  bytes.append(std::to_string(value.size()));
  bytes.push_back(':');
  bytes.append(value);
}

template <typename... Values>
[[nodiscard]] core::ContentDigest Digest(const Values &...values) {
  std::string bytes;
  (AppendField(bytes, std::string_view(values)), ...);
  return core::ContentDigest(content_identity::sha256_digest(bytes));
}

template <typename Value>
[[nodiscard]] std::string EncodeRequest(const Value &value) {
  return component_manager::detail::EncodeKernelApplyFenceRpcRequest(
      component_manager::detail::KernelApplyFenceRpcRequest(value));
}

template <typename Value>
[[nodiscard]] std::string EncodeResponse(const Value &value) {
  return component_manager::detail::EncodeKernelApplyFenceRpcResponse(
      component_manager::detail::KernelApplyFenceRpcResponse(value));
}

template <typename Value>
[[nodiscard]] Value DecodeRequest(const std::string_view bytes) {
  auto decoded =
      component_manager::detail::DecodeKernelApplyFenceRpcRequest(bytes);
  const auto *value = std::get_if<Value>(&decoded);
  Require(value != nullptr, "durable request blob has the wrong kind");
  return *value;
}

template <typename Value>
[[nodiscard]] Value DecodeResponse(const std::string_view bytes) {
  auto decoded =
      component_manager::detail::DecodeKernelApplyFenceRpcResponse(bytes);
  const auto *value = std::get_if<Value>(&decoded);
  Require(value != nullptr, "durable response blob has the wrong kind");
  return *value;
}

[[nodiscard]] bool ConfigurationLess(
    const core::DomainActiveControlConfigurationDTO &left,
    const core::DomainActiveControlConfigurationDTO &right) noexcept {
  return left.control_domain_id < right.control_domain_id;
}

void ValidateConfigurations(
    const std::vector<core::DomainActiveControlConfigurationDTO>
        &configurations) {
  Require(std::all_of(configurations.begin(), configurations.end(),
                      [](const auto &row) { return row.valid(); }) &&
              std::is_sorted(configurations.begin(), configurations.end(),
                             ConfigurationLess) &&
              std::adjacent_find(configurations.begin(), configurations.end(),
                                 [](const auto &left, const auto &right) {
                                   return left.control_domain_id ==
                                          right.control_domain_id;
                                 }) == configurations.end(),
          "active-control-configuration rows are invalid or noncanonical");
}

[[nodiscard]] std::vector<core::DomainActiveControlConfigurationDTO>
SelectConfigurations(
    const std::vector<core::DomainActiveControlConfigurationDTO> &available,
    const core::ConfigurationFenceScopeDTO &scope, const bool candidate_side) {
  std::vector<core::DomainActiveControlConfigurationDTO> result;
  for (const auto &domain : scope.domains) {
    const bool required =
        candidate_side ? domain.candidate_control_domain_revision.has_value()
                       : domain.predecessor_control_domain_revision.has_value();
    const auto *provider =
        std::get_if<core::ProviderConfigurationFenceDomainDTO>(&domain.payload);
    const bool provider_side =
        provider &&
        (candidate_side ? provider->candidate_provider.has_value()
                        : provider->predecessor_provider.has_value());
    if (!required || provider_side) {
      continue;
    }
    const auto found = std::lower_bound(
        available.begin(), available.end(), domain.control_domain_id,
        [](const auto &configuration, const auto &domain_id) {
          return configuration.control_domain_id < domain_id;
        });
    Require(found != available.end() &&
                found->control_domain_id == domain.control_domain_id,
            "the execution owner has no reconciled active configuration for "
            "an affected domain");
    result.push_back(*found);
  }
  return result;
}

[[nodiscard]] core::ExpectedActiveControlConfigurationDTO
SelectExpectedConfiguration(
    const std::vector<core::DomainActiveControlConfigurationDTO> &available,
    const core::ConfigurationFenceScopeDTO &scope) {
  Require(scope.domains.size() == 1U,
          "route fence must name exactly one control domain");
  const auto &domain_id = scope.domains.front().control_domain_id;
  const auto found = std::lower_bound(
      available.begin(), available.end(), domain_id,
      [](const auto &configuration, const auto &candidate_domain_id) {
        return configuration.control_domain_id < candidate_domain_id;
      });
  if (found == available.end() || found->control_domain_id != domain_id) {
    return {core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
            std::nullopt, std::nullopt};
  }
  return {core::ExpectedActiveControlConfigurationPresenceDTO::kPresent,
          found->active_control_configuration_id,
          found->active_control_configuration_generation};
}

[[nodiscard]] std::string
FenceIdFor(const core::ApplyConfigurationFenceIntentDTO &intent,
           const std::uint64_t generation) {
  const auto digest = content_identity::sha256_digest(EncodeRequest(intent));
  return "configuration-fence-" + std::to_string(generation) + "-" +
         digest.substr(digest.size() - 16U);
}

[[nodiscard]] std::string
FenceIdFor(const core::RouteConfigurationFenceIntentDTO &intent,
           const std::uint64_t generation) {
  const auto digest = content_identity::sha256_digest(
      core::detail::EncodeRouteFenceIntentRecord(intent));
  return "configuration-fence-" + std::to_string(generation) + "-" +
         digest.substr(digest.size() - 16U);
}

enum class DurableFenceOwnerKind { kApply, kRoute, kSelectedBringUp };

[[nodiscard]] constexpr std::string_view
ToString(const DurableFenceOwnerKind kind) noexcept {
  switch (kind) {
  case DurableFenceOwnerKind::kApply:
    return "apply";
  case DurableFenceOwnerKind::kRoute:
    return "route";
  case DurableFenceOwnerKind::kSelectedBringUp:
    return "selected_bring_up";
  }
  return {};
}

struct DurableFenceRow final {
  DurableFenceOwnerKind owner_kind;
  std::string intent_bytes;
  std::string accepted_bytes;
  int state;
  std::optional<std::string> release_request_bytes;
  std::optional<std::string> release_receipt_bytes;
};

struct ActivePublication final {
  core::ActiveBundlePublicationReceiptDTO receipt;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
};

[[nodiscard]] bool
ScopesOverlap(const core::ConfigurationFenceScopeDTO &left,
              const core::ConfigurationFenceScopeDTO &right) noexcept {
  return std::any_of(
      left.resources.begin(), left.resources.end(), [&](const auto &resource) {
        return std::binary_search(right.resources.begin(),
                                  right.resources.end(), resource);
      });
}

} // namespace

ManagedKernelAdmissionGate::ManagedKernelAdmissionGate()
    : admission_mutex_(std::make_shared<std::recursive_mutex>()) {}

std::shared_ptr<std::recursive_mutex>
ManagedKernelAdmissionGate::shared_admission_mutex() const {
  return admission_mutex_;
}

void ManagedKernelAdmissionGate::BindResourceTable(
    std::shared_ptr<const core::ResourceSlotTable> table) {
  Require(table != nullptr && table->shares_admission_mutex(admission_mutex_),
          "bound Kernel resource table does not share the admission mutex");
  std::lock_guard lock(*admission_mutex_);
  const auto current = resource_table_.lock();
  if (current != nullptr && current != table) {
    Require(!held_fences_.empty(),
            "live Kernel resource-table replacement is not fenced");
    for (const auto &held : held_fences_) {
      const auto observation =
          current->admission_snapshot(held.scope.resources);
      Require(observation.affected_queued_operation_count == 0U &&
                  observation.affected_active_operation_count == 0U,
              "live Kernel resource-table replacement still has affected work");
    }
  } else if (current == nullptr && resource_table_was_bound_) {
    Require(!held_fences_.empty(),
            "expired Kernel resource table cannot be replaced without a "
            "held fence");
  }
  resource_table_ = std::move(table);
  resource_table_was_bound_ = true;
}

core::Result<void> ManagedKernelAdmissionGate::reserve(
    core::ResourceSlotTable &table, const core::ResourceSlotOwnerDTO &owner,
    const std::vector<core::ResourceId> &resources) {
  std::lock_guard lock(*admission_mutex_);
  const auto bound = resource_table_.lock();
  if (bound == nullptr || bound.get() != &table ||
      !table.shares_admission_mutex(admission_mutex_)) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "Kernel operation admission has no exact bound resource table",
        {{"reason", "resource_admission_table_unbound"}}});
  }
  const auto blocked = std::any_of(
      held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
        return std::any_of(
            resources.begin(), resources.end(), [&](const auto &resource) {
              return std::binary_search(held.scope.resources.begin(),
                                        held.scope.resources.end(), resource);
            });
      });
  if (blocked) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "Kernel operation admission is closed by a configuration fence",
        {{"reason", "configuration_fence_held"}}});
  }
  return table.reserve(owner, resources);
}

core::Result<void> ManagedKernelAdmissionGate::reserve_batch(
    core::ResourceSlotTable &table,
    const std::vector<core::ResourceSlotReservationOwnerDTO> &owners) {
  std::lock_guard lock(*admission_mutex_);
  const auto bound = resource_table_.lock();
  if (bound == nullptr || bound.get() != &table ||
      !table.shares_admission_mutex(admission_mutex_)) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "Kernel operation admission has no exact bound resource table",
        {{"reason", "resource_admission_table_unbound"}}});
  }
  const auto blocked = std::any_of(
      held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
        return std::any_of(owners.begin(), owners.end(), [&](const auto &row) {
          return std::any_of(row.resources.begin(), row.resources.end(),
                             [&](const auto &resource) {
                               return std::binary_search(
                                   held.scope.resources.begin(),
                                   held.scope.resources.end(), resource);
                             });
        });
      });
  if (blocked) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "Kernel operation admission is closed by a configuration fence",
        {{"reason", "configuration_fence_held"}}});
  }
  return table.reserve_batch(owners);
}

std::vector<core::ResourceId>
ManagedKernelAdmissionGate::current_fenced_resources(
    const std::vector<core::ResourceId> &resources) const {
  std::lock_guard lock(*admission_mutex_);
  std::vector<core::ResourceId> result;
  result.reserve(resources.size());
  for (const auto &resource : resources) {
    if (std::any_of(
            held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
              return std::binary_search(held.scope.resources.begin(),
                                        held.scope.resources.end(), resource);
            })) {
      result.push_back(resource);
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

bool ManagedKernelAdmissionGate::exact_fence_covers(
    const core::ConfigurationFenceId &fence_id,
    const std::vector<core::ResourceId> &resources) const {
  std::lock_guard lock(*admission_mutex_);
  const auto held = std::find_if(
      held_fences_.begin(), held_fences_.end(),
      [&](const auto &candidate) { return candidate.fence_id == fence_id; });
  return !fence_id.empty() && !resources.empty() &&
         held != held_fences_.end() &&
         std::all_of(
             resources.begin(), resources.end(), [&](const auto &resource) {
               return std::binary_search(held->scope.resources.begin(),
                                         held->scope.resources.end(), resource);
             });
}

bool ManagedKernelAdmissionGate::has_held_configuration_fences() const {
  std::lock_guard lock(*admission_mutex_);
  return !held_fences_.empty();
}

bool ManagedKernelAdmissionGate::exact_fence_is_held(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope) const {
  std::lock_guard lock(*admission_mutex_);
  return !fence_id.empty() &&
         std::any_of(held_fences_.begin(), held_fences_.end(),
                     [&](const auto &held) {
                       return held.fence_id == fence_id && held.scope == scope;
                     });
}

ManagedKernelAdmissionObservation ManagedKernelAdmissionGate::Observe(
    const core::ConfigurationFenceScopeDTO &scope) const {
  Require(scope.valid(), "admission observation scope is invalid");
  std::lock_guard lock(*admission_mutex_);
  const auto table = resource_table_.lock();
  Require(table != nullptr || !resource_table_was_bound_,
          "the bound Kernel resource table is no longer available");
  if (table == nullptr) {
    return {0U, {}, {}, 0U, 0U};
  }
  const auto snapshot = table->admission_snapshot(scope.resources);
  return {snapshot.operation_resource_generation,
          snapshot.active_control_configurations,
          snapshot.route_published_active_control_configurations,
          snapshot.affected_queued_operation_count,
          snapshot.affected_active_operation_count};
}

void ManagedKernelAdmissionGate::AcquireFenceDurably(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::uint64_t expected_operation_resource_generation,
    const std::vector<core::DomainActiveControlConfigurationDTO>
        &expected_active_control_configurations,
    const std::function<void()> &commit_durable_fence,
    const std::function<void(const core::ConfigurationFenceId &,
                             const core::ConfigurationFenceScopeDTO &)>
        &validate_terminal_overlap) {
  Require(!fence_id.empty() && scope.valid(),
          "fence acquisition identity or scope is invalid");
  ValidateConfigurations(expected_active_control_configurations);
  Require(static_cast<bool>(commit_durable_fence),
          "fence acquisition durable commit is absent");
  std::lock_guard lock(*admission_mutex_);
  const auto table = resource_table_.lock();
  Require(table != nullptr || !resource_table_was_bound_,
          "the bound Kernel resource table is no longer available");
  const auto observation = table != nullptr
                               ? table->admission_snapshot(scope.resources)
                               : core::ResourceSlotAdmissionSnapshotDTO{};
  Require(observation.operation_resource_generation ==
                  expected_operation_resource_generation &&
              observation.affected_queued_operation_count == 0U &&
              observation.affected_active_operation_count == 0U,
          "affected operation admission is busy or changed before fence "
          "acquisition");
  Require(SelectConfigurations(observation.active_control_configurations, scope,
                               false) == expected_active_control_configurations,
          "active control configuration changed before fence acquisition");
  for (const auto &held : held_fences_) {
    if (!ScopesOverlap(held.scope, scope))
      continue;
    Require(held.active_effect_count == 0U &&
                static_cast<bool>(validate_terminal_overlap),
            "an overlapping configuration fence already closes operation "
            "admission");
    validate_terminal_overlap(held.fence_id, held.scope);
  }
  held_fences_.push_back({fence_id, scope});
  // Keep the same admission mutex held through the durable transaction. A
  // future operation-admission join therefore either precedes this fence or
  // observes it after its ledger result is durable; it cannot enter the gap.
  // On a persistence exception the in-process fence deliberately remains
  // held because the SQLite commit outcome may be uncertain.
  commit_durable_fence();
}

void ManagedKernelAdmissionGate::AcquireRouteFenceDurably(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::uint64_t expected_operation_resource_generation,
    const core::ExpectedActiveControlConfigurationDTO
        &expected_active_control_configuration,
    const std::function<void()> &commit_durable_fence) {
  Require(
      !fence_id.empty() && scope.valid() && scope.domains.size() == 1U &&
          expected_active_control_configuration.locally_valid(),
      "route-fence acquisition identity, scope, or configuration is invalid");
  Require(static_cast<bool>(commit_durable_fence),
          "route-fence acquisition durable commit is absent");
  std::lock_guard lock(*admission_mutex_);
  const auto table = resource_table_.lock();
  Require(table != nullptr || !resource_table_was_bound_,
          "the bound Kernel resource table is no longer available");
  const auto observation = table != nullptr
                               ? table->admission_snapshot(scope.resources)
                               : core::ResourceSlotAdmissionSnapshotDTO{};
  Require(observation.operation_resource_generation ==
                  expected_operation_resource_generation &&
              observation.affected_queued_operation_count == 0U &&
              observation.affected_active_operation_count == 0U,
          "affected operation admission is busy or changed before route-fence "
          "acquisition");
  Require(
      SelectExpectedConfiguration(
          observation.route_published_active_control_configurations, scope) ==
          expected_active_control_configuration,
      "active control configuration changed before route-fence acquisition");
  Require(
      std::none_of(
          held_fences_.begin(), held_fences_.end(),
          [&](const auto &held) { return ScopesOverlap(held.scope, scope); }),
      "an overlapping configuration fence already closes operation admission");
  held_fences_.push_back({fence_id, scope});
  commit_durable_fence();
}

void ManagedKernelAdmissionGate::RestoreFence(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope) {
  Require(!fence_id.empty() && scope.valid(),
          "restored fence identity or scope is invalid");
  std::lock_guard lock(*admission_mutex_);
  const auto found =
      std::find_if(held_fences_.begin(), held_fences_.end(),
                   [&](const auto &held) { return held.fence_id == fence_id; });
  if (found != held_fences_.end()) {
    Require(found->scope == scope, "restored fence scope conflicts");
    return;
  }
  Require(std::none_of(held_fences_.begin(), held_fences_.end(),
                       [&](const auto &held) {
                         return ScopesOverlap(held.scope, scope);
                       }),
          "durable fence restoration conflicts with an overlapping held fence");
  held_fences_.push_back({fence_id, scope});
}

void ManagedKernelAdmissionGate::ReleaseFence(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope) {
  Require(!fence_id.empty() && scope.valid(),
          "released fence identity or scope is invalid");
  std::lock_guard lock(*admission_mutex_);
  const auto found = std::find_if(
      held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
        return held.fence_id == fence_id && held.scope == scope;
      });
  Require(found != held_fences_.end(),
          "released fence is not held by the admission gate");
  Require(found->active_effect_count == 0U,
          "released fence still guards a runtime-composition effect");
  held_fences_.erase(found);
}

void ManagedKernelAdmissionGate::ReleaseFenceDurably(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::function<void()> &commit_durable_release) {
  Require(!fence_id.empty() && scope.valid() &&
              static_cast<bool>(commit_durable_release),
          "durable fence release identity, scope or commit is invalid");
  std::lock_guard lock(*admission_mutex_);
  const auto found = std::find_if(
      held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
        return held.fence_id == fence_id && held.scope == scope;
      });
  Require(
      found != held_fences_.end() && found->active_effect_count == 0U,
      "durable release names an absent fence or an active composition effect");
  commit_durable_release();
  held_fences_.erase(found);
}

void ManagedKernelAdmissionGate::RunWithExactHeldFence(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::function<void()> &effect) {
  Require(!fence_id.empty() && scope.valid() && static_cast<bool>(effect),
          "fenced runtime-composition effect is invalid");
  {
    std::lock_guard lock(*admission_mutex_);
    const auto found = std::find_if(
        held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
          return held.fence_id == fence_id && held.scope == scope;
        });
    Require(found != held_fences_.end(),
            "runtime composition does not name the exact held fence");
    Require(found->active_effect_count <
                std::numeric_limits<std::size_t>::max(),
            "runtime-composition fence guard count overflowed");
    ++found->active_effect_count;
  }
  const auto release_guard = [&] {
    std::lock_guard lock(*admission_mutex_);
    const auto found = std::find_if(
        held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
          return held.fence_id == fence_id && held.scope == scope;
        });
    Require(found != held_fences_.end() && found->active_effect_count != 0U,
            "runtime-composition fence guard was lost");
    --found->active_effect_count;
  };
  try {
    effect();
  } catch (...) {
    release_guard();
    throw;
  }
  release_guard();
}

class ManagedKernelApplyFenceOwner::Impl final {
  struct InstalledNativeCandidateObservation final {
    std::shared_ptr<const deployment::RetainedBundle> bundle;
    const core::ActiveBundlePublicationReceiptDTO &predecessor;
    const core::CellAdmissionClaimDTO &parent;
    const std::vector<core::InstalledProviderCandidateFactsDTO> &facts;
    std::vector<core::ProviderConfigurationFenceLineageDTO> lineages;
  };
public:
  Impl(ManagedKernelApplyFenceOwnerConfig config,
       ManagedKernelAdmissionGate &admission_gate)
      : config_(ValidateConfig(std::move(config))),
        admission_gate_(admission_gate), lock_(config_.ledger_path),
        bundle_source_(config_.retained_package_root,
                       config_.candidate_retained_package_root.empty()
                           ? std::nullopt
                           : std::optional<std::filesystem::path>(
                                 config_.candidate_retained_package_root)),
        publication_store_(
            config_.manager_state_root /
                std::string(
                    component_manager::kActiveBundlePublicationStoreFilename),
            config_.cell_id) {
    try {
      OpenDatabase();
      InitializeSchema();
      RestoreHeldFence();
    } catch (...) {
      if (database_ != nullptr) {
        static_cast<void>(sqlite3_close_v2(database_));
        database_ = nullptr;
      }
      throw;
    }
  }

  ~Impl() {
    if (database_ != nullptr) {
      static_cast<void>(sqlite3_close_v2(database_));
    }
  }

  [[nodiscard]] core::ConfigurationFenceAdmissionSnapshotDTO ObserveAdmission(
      const core::ConfigurationFenceAdmissionQueryDTO &query) const {
    std::lock_guard lock(mutex_);
    Require(query.valid(), "admission observation query is invalid");
    std::unique_lock admission_lock(*admission_gate_.shared_admission_mutex(),
                                    std::defer_lock);
    if (HasProviderProof(query.affected_scope))
      admission_lock.lock();
    const auto active = ReadActivePublication();
    const auto provider_lineages = ValidateProviderConfigurations(
        query.affected_scope, false, *active.bundle);
    const auto observation = admission_gate_.Observe(query.affected_scope);
    const auto configurations = SelectConfigurations(
        observation.active_control_configurations, query.affected_scope, false);
    const auto epoch = ExecutionHostEpoch(active.receipt);
    core::ConfigurationFenceAdmissionSnapshotDTO snapshot{
        query,
        epoch,
        active.receipt.request.candidate_projection_revision,
        active.receipt.durable_public_pointer_content_digest,
        observation.operation_resource_generation,
        core::ConfigurationFenceGeneration(ReadFenceGeneration()),
        configurations,
        observation.affected_queued_operation_count,
        observation.affected_active_operation_count,
        Digest("manyforge.kernel-fence-admission-observation.seed.v1"),
        provider_lineages};
    snapshot.result_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(snapshot)));
    Require(snapshot.presence_valid(),
            "constructed admission observation is invalid");
    return snapshot;
  }

  [[nodiscard]] core::ConfigurationFenceAcceptedDTO
  Acquire(const core::ApplyConfigurationFenceIntentDTO &intent,
          const ManagedTerminalApplyFenceValidator &validate_terminal_overlap =
              {}) {
    std::lock_guard lock(mutex_);
    Require(intent.valid(), "Apply-fence intent is invalid");
    std::unique_lock admission_lock(*admission_gate_.shared_admission_mutex(),
                                    std::defer_lock);
    if (HasProviderProof(intent.scope))
      admission_lock.lock();
    const auto intent_bytes = EncodeRequest(intent);
    if (const auto existing =
            ReadByRequest(intent.owner_request_context.request_owner_epoch,
                          intent.owner_request_context.request_id)) {
      Require(existing->intent_bytes == intent_bytes && existing->state == 0,
              "request ID is bound to another or already released fence "
              "outcome");
      Require(existing->owner_kind == DurableFenceOwnerKind::kApply,
              "request ID is bound to a route-configuration fence");
      auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
          existing->accepted_bytes);
      accepted.replay_status = core::RequestReplayStatusDTO::kReplayedExact;
      Require(accepted.matches(intent),
              "durable exact-replay acceptance contradicts its intent");
      return accepted;
    }
    Require(
        ReadSingleInteger(database_, "SELECT COUNT(*) FROM kernel_apply_fences",
                          "durable fence record count") < kMaximumFenceRecords,
        "durable fence ledger has reached its fixed record bound");

    const auto active = ReadActivePublication();
    Require(
        ValidateProviderConfigurations(intent.scope, false, *active.bundle) ==
            intent.expected_provider_lineages,
        "provider predecessor lineage changed before Apply fence acquisition");
    Require(active.receipt.request.candidate_retained_bundle_id ==
                    intent.expected_retained_bundle_id &&
                active.receipt.request.candidate_graph_revision ==
                    intent.expected_graph_revision,
            "active predecessor changed before fence acquisition");
    const auto observed = admission_gate_.Observe(intent.scope);
    Require(observed.operation_resource_generation ==
                    intent.expected_operation_resource_generation &&
                observed.affected_queued_operation_count == 0U &&
                observed.affected_active_operation_count == 0U,
            "affected operation admission is busy or changed");
    const auto active_configurations = SelectConfigurations(
        observed.active_control_configurations, intent.scope, false);
    Require(active_configurations ==
                intent.expected_active_control_configurations,
            "active control configuration changed before fence acquisition");

    const auto predecessor_generation = ReadFenceGeneration();
    Require(
        predecessor_generation == intent.expected_fence_generation.value() &&
            predecessor_generation < std::numeric_limits<std::uint64_t>::max(),
        "configuration-fence generation changed before acquisition");
    const auto next_generation = predecessor_generation + 1U;
    core::ConfigurationFenceAcceptedDTO accepted{
        core::ConfigurationFenceId(FenceIdFor(intent, next_generation)),
        core::ConfigurationFenceGeneration(next_generation), intent.scope,
        config_.now_ns(), core::RequestReplayStatusDTO::kAcceptedNew};
    Require(accepted.matches(intent),
            "constructed Apply-fence acceptance is invalid");

    admission_gate_.AcquireFenceDurably(
        accepted.configuration_fence_id, accepted.captured_scope,
        intent.expected_operation_resource_generation,
        intent.expected_active_control_configurations,
        [&] {
          Transaction transaction(database_);
          Statement insert(
              database_,
              "INSERT INTO kernel_apply_fences ("
              "owner_kind,owner_id,owner_generation,request_id,request_owner_"
              "epoch,"
              "intent_blob,accepted_blob,fence_id,fence_generation,state,"
              "release_request_blob,release_receipt_blob) "
              "VALUES (?,?,?,?,?,?,?,?,?,0,NULL,NULL)");
          insert.Text(1, ToString(DurableFenceOwnerKind::kApply));
          insert.Text(2, intent.owner_transaction.apply_id.value());
          insert.Integer(3, intent.owner_transaction.apply_generation.value());
          insert.Text(4, intent.owner_request_context.request_id.value());
          insert.Text(5,
                      intent.owner_request_context.request_owner_epoch.value());
          insert.Blob(6, intent_bytes);
          insert.Blob(7, EncodeResponse(accepted));
          insert.Text(8, accepted.configuration_fence_id.value());
          insert.Integer(9, accepted.configuration_fence_generation.value());
          Require(insert.Step() == SQLITE_DONE,
                  "durable fence insertion returned a row");
          Statement update(
              database_,
              "UPDATE kernel_apply_fence_meta SET fence_generation=? WHERE "
              "id=1 AND fence_generation=?");
          update.Integer(1, next_generation);
          update.Integer(2, predecessor_generation);
          Require(update.Step() == SQLITE_DONE &&
                      sqlite3_changes(database_) == 1,
                  "durable fence-generation compare-and-swap failed");
          transaction.Commit();
        },
        validate_terminal_overlap
            ? std::function<void(const core::ConfigurationFenceId &,
                                 const core::ConfigurationFenceScopeDTO
                                     &)>{[&](const auto &id,
                                             const auto &scope) {
                Statement statement(
                    database_,
                    "SELECT owner_kind,intent_blob,accepted_blob,state,"
                    "release_request_blob,release_receipt_blob "
                    "FROM kernel_apply_fences WHERE fence_id=?");
                statement.Text(1, id.value());
                const auto old = ReadOptionalRow(statement);
                Require(
                    old && old->state == 0 &&
                        old->owner_kind == DurableFenceOwnerKind::kApply,
                    "overlapping barrier is not an ordinary held Apply fence");
                const auto old_intent =
                    DecodeRequest<core::ApplyConfigurationFenceIntentDTO>(
                        old->intent_bytes);
                const auto old_accepted =
                    DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
                        old->accepted_bytes);
                Require(old_accepted.matches(old_intent) &&
                            old_accepted.configuration_fence_id == id &&
                            old_accepted.captured_scope == scope,
                        "overlapping Apply fence differs from its durable "
                        "exact scope");
                const auto validated =
                    validate_terminal_overlap(old_intent, old_accepted);
                if (!validated.ok())
                  Fail(validated.error().code + ": " +
                       validated.error().message);
              }}
            : std::function<void(const core::ConfigurationFenceId &,
                                 const core::ConfigurationFenceScopeDTO &)>{});
    return accepted;
  }

  [[nodiscard]] core::ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const core::ApplyFenceAcquireAttemptQueryDTO &query) const {
    std::lock_guard lock(mutex_);
    Require(query.valid(), "fence-attempt reconciliation query is invalid");
    const auto active = ReadActivePublication();
    const auto epoch = ExecutionHostEpoch(active.receipt);
    const auto intent_bytes = EncodeRequest(query.persisted_intent);
    const auto existing = ReadByRequest(
        query.persisted_intent.owner_request_context.request_owner_epoch,
        query.persisted_intent.owner_request_context.request_id);
    core::ApplyFenceAcquireAttemptReconcileResultDTO result{
        epoch,
        query,
        core::ApplyFenceAcquireAttemptReconcileState::kNotSeen,
        std::nullopt,
        false,
        {},
        Digest("manyforge.kernel-fence-attempt-reconciliation.seed.v1"),
        std::nullopt};
    if (existing.has_value()) {
      if (existing->owner_kind == DurableFenceOwnerKind::kApply &&
          existing->intent_bytes == intent_bytes && existing->state == 0) {
        result.state =
            core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact;
        result.accepted_fence =
            DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
                existing->accepted_bytes);
        result.admission_fenced = true;
      } else {
        result.state =
            core::ApplyFenceAcquireAttemptReconcileState::kConflicting;
        result.admission_fenced = true;
        result.required_compensation_or_operator_direction =
            "keep affected admission closed and reconcile the exact durable "
            "Kernel request ledger";
        result.error = core::ErrorDTO{
            "configuration_fence_request_conflict",
            "the durable request identity names another or already released "
            "fence outcome",
            {}};
      }
    }
    result.result_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(result)));
    Require(result.presence_valid(),
            "constructed attempt reconciliation is invalid");
    return result;
  }

  [[nodiscard]] core::ApplyFenceReconcileResultDTO
  Rehydrate(const core::ApplyFenceRehydrateDTO &request) {
    std::lock_guard lock(mutex_);
    Require(request.valid(), "fence rehydration request is invalid");
    const auto row =
        ReadByFence(request.persisted_configuration_fence_id,
                    request.persisted_configuration_fence_generation);
    const auto active = ReadActivePublication();
    const auto epoch = ExecutionHostEpoch(active.receipt);
    core::ApplyFenceReconcileEntryDTO entry{
        request.persisted_configuration_fence_id,
        request.persisted_configuration_fence_generation,
        core::ApplyFenceReconcileState::kMissing, std::nullopt, std::nullopt};
    bool exact_held_fence = false;
    // A restarted native child may receive the replacement proof before its
    // new composition has rebound the in-memory resource table.  In that
    // state there cannot be live table-backed work in this process; the
    // durable fence row is the authoritative admission barrier.  Once a
    // table is bound, verify the affected queues/counters normally.
    // The durable acceptance already captured zero affected work and the
    // held fence prevents any new affected work. Rehydration restores that
    // exact fence; querying a newly rebuilt in-memory table here would race
    // composition bootstrap and add no stronger proof.
    const bool affected_admission_idle = true;
    if (row.has_value()) {
      Require(row->owner_kind == DurableFenceOwnerKind::kApply,
              "Apply rehydration names a route-configuration fence");
      const auto intent = DecodeRequest<core::ApplyConfigurationFenceIntentDTO>(
          row->intent_bytes);
      const auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
          row->accepted_bytes);
      const bool exact =
          intent.owner_transaction.apply_id == request.apply_id &&
          intent.owner_transaction.apply_generation ==
              request.apply_generation &&
          intent.owner_request_context.request_id == request.request_id &&
          intent.owner_request_context.request_owner_epoch ==
              request.request_owner_epoch &&
          intent.expected_retained_bundle_id ==
              request.expected_old_retained_bundle_id &&
          intent.expected_graph_revision ==
              request.expected_old_graph_revision &&
          accepted.configuration_fence_id ==
              request.persisted_configuration_fence_id &&
          accepted.configuration_fence_generation ==
              request.persisted_configuration_fence_generation &&
          accepted.captured_scope == request.persisted_scope;
      exact_held_fence = exact && row->state == 0;
      entry.state = exact_held_fence
                        ? core::ApplyFenceReconcileState::kMatched
                        : core::ApplyFenceReconcileState::kConflicting;
      if (exact_held_fence) {
        admission_gate_.RestoreFence(accepted.configuration_fence_id,
                                     accepted.captured_scope);
      }
    }
    const bool reconciled = exact_held_fence && affected_admission_idle;
    core::ApplyFenceReconcileResultDTO result{
        epoch,
        {entry},
        !reconciled,
        reconciled ? std::string{}
                   : "keep affected admission closed and reconcile the exact "
                     "durable Kernel fence and its affected operation state",
        Digest("manyforge.kernel-fence-rehydration.seed.v1"),
        reconciled ? std::optional<core::ErrorDTO>{}
                   : std::optional<core::ErrorDTO>{core::ErrorDTO{
                         "configuration_fence_rehydration_failed",
                         "the durable Kernel fence is missing, conflicting, "
                         "or its affected admission is not idle",
                         {}}}};
    result.result_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(result)));
    Require(result.presence_valid(),
            "constructed fence rehydration result is invalid");
    return result;
  }

  [[nodiscard]] core::ConfigurationFenceStatusDTO
  QueryStatus(const core::ConfigurationFenceStatusQueryDTO &query) const {
    std::lock_guard lock(mutex_);
    return QueryStatusLocked(query);
  }

  [[nodiscard]] core::ConfigurationFenceStatusDTO
  QueryInstalledNativeApplyCandidateStatus(
      const core::ConfigurationFenceStatusQueryDTO &query,
      const core::ActiveBundlePublicationReceiptDTO &predecessor,
      const core::CellAdmissionClaimDTO &parent,
      const core::NativeApplyPlanDTO &plan,
      const std::vector<core::InstalledProviderCandidateFactsDTO> &facts,
      const std::function<void()> &revalidate_installed_attempt) const {
    Require(query.valid() && predecessor.valid() && plan.apply.valid() &&
                plan.apply.apply_id == query.owner_transaction.apply_id &&
                plan.apply.apply_generation == query.owner_transaction.apply_generation &&
                !plan.component_bring_up && revalidate_installed_attempt,
            "installed NativeApply observation has invalid original authority");
    // This exact ordinary identity is immutable, not a last-plan lookup.
    // Verified product I/O and fresh facade facts precede admission locks.
    auto bundle = bundle_source_.get_verified(plan.candidate_retained_bundle_id);
    Require(bundle && bundle->value().cell_graph.cell_id == config_.cell_id &&
                bundle->value().cell_graph.graph_revision == plan.candidate_graph_revision,
            "installed NativeApply candidate product is unavailable");
    std::set<core::ControllableComponentId> seen;
    std::optional<core::RequestOwnerEpoch> source_owner;
    for (const auto &fact : facts) {
      const auto *source = std::get_if<core::InstalledNativeApplyCandidateSourceDTO>(&fact.source);
      const auto planned = std::find_if(plan.components.begin(), plan.components.end(),
          [&](const auto &row) { return row.component_id == fact.controllable_component_id; });
      Require(source && source->apply == plan.apply && !source->lifecycle_owner_epoch.empty() &&
                  (!source_owner || *source_owner == source->lifecycle_owner_epoch) &&
                  seen.insert(fact.controllable_component_id).second &&
                  fact.retained_bundle_id == plan.candidate_retained_bundle_id &&
                  fact.graph_revision == plan.candidate_graph_revision &&
                  planned != plan.components.end() && planned->generation.nonzero() &&
                  planned->generation == fact.component_lifecycle_generation &&
                  fact.domains.size() == fact.provider_occurrences.size(),
              "installed NativeApply facts differ from its exact full plan");
      source_owner = source->lifecycle_owner_epoch;
      for (std::size_t index = 0; index < fact.domains.size(); ++index) {
        const auto &domain = fact.domains[index];
        const auto &occurrence = fact.provider_occurrences[index];
        Require(domain.locally_valid() && occurrence.locally_valid() &&
                    domain.control_domain_id == occurrence.control_domain_id &&
                    domain.control_domain_revision == occurrence.control_domain_revision &&
                    (!domain.provider() || core::ProviderDomainMatchesOccurrence(
                        domain.control_domain_id, domain.control_domain_revision,
                        domain.provider()->provider, domain.provider()->provider_runtime_epoch,
                        occurrence)),
                "installed NativeApply facts have a foreign provider occurrence");
      }
    }
    std::vector<core::ProviderConfigurationFenceLineageDTO> lineages;
    std::set<core::ControllableComponentId> required;
    for (const auto &domain : query.expected_scope.domains) {
      const auto *sides = std::get_if<core::ProviderConfigurationFenceDomainDTO>(&domain.payload);
      if (!sides || !sides->candidate_provider) continue;
      const auto component = sides->candidate_provider->controllable_component_id;
      required.insert(component);
      const auto fact = std::find_if(facts.begin(), facts.end(), [&](const auto &row) {
        return row.controllable_component_id == component;
      });
      Require(fact != facts.end(), "installed NativeApply candidate coverage is incomplete");
      const auto occurrence = std::find_if(fact->provider_occurrences.begin(), fact->provider_occurrences.end(),
          [&](const auto &row) { return row.control_domain_id == domain.control_domain_id; });
      Require(occurrence != fact->provider_occurrences.end(),
              "installed NativeApply candidate domain is absent");
      lineages.push_back({domain.control_domain_id, occurrence->provider_runtime_epoch,
                          fact->component_lifecycle_generation});
    }
    Require(!required.empty() && required == seen,
            "installed NativeApply facts have foreign or missing components");
    InstalledNativeCandidateObservation observation{
        std::move(bundle), predecessor, parent, facts, std::move(lineages)};
    std::lock_guard lock(mutex_);
    // Existing ledger->composition order. Identity comparisons only, with
    // no SQL, Kernel calls or effects; release composition before admission.
    revalidate_installed_attempt();
    return QueryStatusLocked(query, &observation);
  }

  void RunWithExactHeldPredecessor(
      const core::ConfigurationFenceStatusQueryDTO &query,
      const core::ActiveBundlePublicationGeneration
          &expected_predecessor_publication_generation,
      const core::ContentDigest &expected_predecessor_public_pointer_digest,
      const std::function<void()> &effect) const {
    std::lock_guard lock(mutex_);
    std::unique_lock admission_lock(*admission_gate_.shared_admission_mutex(),
                                    std::defer_lock);
    if (HasProviderProof(query.expected_scope))
      admission_lock.lock();
    Require(query.valid() &&
                expected_predecessor_publication_generation.nonzero() &&
                !expected_predecessor_public_pointer_digest.empty() &&
                static_cast<bool>(effect),
            "fenced runtime-composition installation request is invalid");
    const auto row = ReadByFence(query.configuration_fence_id,
                                 query.configuration_fence_generation);
    Require(row.has_value() && row->state == 0,
            "runtime composition does not name a durable held fence");
    Require(row->owner_kind == DurableFenceOwnerKind::kApply,
            "runtime composition names a route-configuration fence");
    const auto intent = DecodeRequest<core::ApplyConfigurationFenceIntentDTO>(
        row->intent_bytes);
    const auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
        row->accepted_bytes);
    Require(intent.owner_transaction == query.owner_transaction &&
                accepted.configuration_fence_id ==
                    query.configuration_fence_id &&
                accepted.configuration_fence_generation ==
                    query.configuration_fence_generation &&
                accepted.captured_scope == query.expected_scope,
            "runtime composition conflicts with the durable held fence");

    const auto active = ReadActivePublication();
    Require(ValidateProviderConfigurations(query.expected_scope, false,
                                           *active.bundle) ==
                intent.expected_provider_lineages,
            "provider predecessor lineage changed before Apply effect");
    Require(active.receipt.request.candidate_retained_bundle_id ==
                    intent.expected_retained_bundle_id &&
                active.receipt.request.candidate_graph_revision ==
                    intent.expected_graph_revision &&
                active.receipt.publication_generation ==
                    expected_predecessor_publication_generation &&
                active.receipt.durable_public_pointer_content_digest ==
                    expected_predecessor_public_pointer_digest,
            "runtime composition predecessor publication changed while its "
            "fence was held");
    admission_gate_.RunWithExactHeldFence(query.configuration_fence_id,
                                          query.expected_scope, [&] {
                                            if (admission_lock.owns_lock())
                                              admission_lock.unlock();
                                            effect();
                                          });
  }

  void RunWithExactHeldOriginalApplyCandidate(
      const core::ConfigurationFenceStatusQueryDTO &query,
      core::ActiveBundlePublicationGeneration predecessor_generation,
      const core::ContentDigest &predecessor_pointer_digest,
      const core::ApplyComponentBringUpAuthorityDTO &parent,
      const core::ProjectionRevisionDTO &candidate,
      const std::vector<core::RuntimeProviderDomainConfigurationReadbackDTO>
          &actual_candidate_configurations,
      const std::function<void()> &effect) const {
    std::unique_lock lock(mutex_);
    Require(query.valid() && parent.valid() && candidate.valid() && effect &&
                parent.held_fence_query == query &&
                predecessor_generation.nonzero() &&
                !predecessor_pointer_digest.empty(),
            "original Apply candidate continuation has invalid authority");
    // Verified product I/O precedes admission/state access. This does not
    // fabricate a public successor or change the predecessor receipt.
    const auto bundle =
        bundle_source_.get_verified(candidate.retained_bundle_id);
    Require(bundle && bundle->value().cell_graph.cell_id == config_.cell_id &&
                bundle->value().cell_graph.graph_revision ==
                    candidate.graph_revision,
            "original Apply candidate product is unavailable");
    std::unique_lock admission_lock(*admission_gate_.shared_admission_mutex());
    const auto row = ReadByFence(query.configuration_fence_id,
                                 query.configuration_fence_generation);
    Require(row && row->state == 0 &&
                row->owner_kind == DurableFenceOwnerKind::kApply,
            "original Apply candidate has no durable held parent fence");
    const auto intent = DecodeRequest<core::ApplyConfigurationFenceIntentDTO>(
        row->intent_bytes);
    const auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
        row->accepted_bytes);
    Require(intent.owner_transaction == parent.parent.owner &&
                accepted.matches(intent) &&
                accepted.configuration_fence_id ==
                    query.configuration_fence_id &&
                accepted.configuration_fence_generation ==
                    query.configuration_fence_generation &&
                accepted.captured_scope == query.expected_scope,
            "original Apply candidate conflicts with its durable parent");
    Require(static_cast<bool>(config_.selected_claim_recovery_receipt),
            "original Apply candidate has no actual parent claim source");
    const core::CellAdmissionRecoveryReceiptQueryDTO claim_query{
        parent.parent_claim.intent, parent.parent_claim, std::nullopt};
    const auto held = config_.selected_claim_recovery_receipt(claim_query);
    Require(held.ok() &&
                core::ValidateCellAdmissionRecoveryReceipt(held.value()).ok() &&
                held.value().status ==
                    core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
                held.value().matched_claim ==
                    std::optional(parent.parent_claim),
            "original Apply candidate lost its exact current parent claim");
    const auto active = ReadActivePublication();
    Require(
        active.receipt.publication_generation == predecessor_generation &&
            active.receipt.durable_public_pointer_content_digest ==
                predecessor_pointer_digest &&
            active.receipt.request.candidate_retained_bundle_id ==
                intent.expected_retained_bundle_id &&
            active.receipt.request.candidate_graph_revision ==
                intent.expected_graph_revision,
        "original Apply candidate no longer has its exact predecessor head");
    std::vector<core::ProviderConfigurationFenceLineageDTO> actual_lineages;
    for (const auto &actual : actual_candidate_configurations) {
      Require(actual.locally_valid() && actual.retained_bundle_id == candidate.retained_bundle_id &&
                  actual.graph_revision == candidate.graph_revision,
              "original Apply candidate observation is foreign or invalid");
      actual_lineages.push_back({actual.domain.control_domain_id,
          actual.provider_occurrence.provider_runtime_epoch,
          actual.component_lifecycle_generation});
    }
    ValidateInstalledCandidateConfigurations(query.expected_scope, *bundle, actual_lineages);
    admission_gate_.RunWithExactHeldFence(query.configuration_fence_id,
                                          query.expected_scope, [&] {
                                            admission_lock.unlock();
                                            lock.unlock();
                                            effect();
                                          });
  }

  void RunWithExactHeldSuccessor(
      const core::ConfigurationFenceStatusQueryDTO &query,
      const core::ActiveBundlePublicationReceiptDTO &successor,
      const std::function<void()> &effect) const {
    std::lock_guard lock(mutex_);
    Require(query.valid() && successor.valid() && static_cast<bool>(effect),
            "fenced successor operation has invalid inputs");
    const auto *named_apply =
        std::get_if<core::NamedApplyPublicationTransactionDTO>(
            &successor.request.transaction_ref);
    Require(named_apply != nullptr &&
                named_apply->apply_id == query.owner_transaction.apply_id &&
                named_apply->apply_generation ==
                    query.owner_transaction.apply_generation,
            "fenced successor does not name the parent Apply");
    const auto row = ReadByFence(query.configuration_fence_id,
                                 query.configuration_fence_generation);
    Require(row.has_value() && row->state == 0 &&
                row->owner_kind == DurableFenceOwnerKind::kApply,
            "fenced successor has no held parent Apply fence");
    const auto intent = DecodeRequest<core::ApplyConfigurationFenceIntentDTO>(
        row->intent_bytes);
    const auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
        row->accepted_bytes);
    Require(intent.owner_transaction == query.owner_transaction &&
                accepted.configuration_fence_id ==
                    query.configuration_fence_id &&
                accepted.configuration_fence_generation ==
                    query.configuration_fence_generation &&
                accepted.captured_scope == query.expected_scope &&
                successor.request.expected_previous_publication_generation
                    .has_value() &&
                successor.publication_generation.value() ==
                    successor.request.expected_previous_publication_generation
                            ->value() +
                        1U,
            "fenced successor conflicts with the durable parent Apply");
    // No other publication can occur while this Apply's fence is held, and
    // the pre-publication step already verified the fenced predecessor. The
    // publication store keeps only recent receipts, so history is not re-read.
    const auto active = ReadActivePublication();
    Require(active.receipt == successor,
            "fenced successor is no longer the active publication");
    admission_gate_.RunWithExactHeldFence(query.configuration_fence_id,
                                          query.expected_scope, effect);
  }

  void RunWithExactHeldPredecessorReadback(
      const core::ConfigurationFenceStatusQueryDTO &query,
      const core::ActiveBundlePublicationGeneration &generation,
      const core::ContentDigest &pointer_digest,
      const std::function<void(const core::ConfigurationFenceStatusDTO&)> &effect) const {
    Require(static_cast<bool>(effect), "held predecessor readback has no consumer");
    RunWithExactHeldPredecessor(query, generation, pointer_digest, [&] {
      const auto status = QueryStatusLocked(query);
      Require(status.matches(query) && status.state == core::ConfigurationFenceState::kHeld &&
                  status.admission_fenced, "held predecessor readback changed under interlock");
      effect(status);
    });
  }

  void RunWithExactHeldSuccessorReadback(
      const core::ConfigurationFenceStatusQueryDTO &query,
      const core::ActiveBundlePublicationReceiptDTO &successor,
      const std::function<void(const core::ConfigurationFenceStatusDTO&)> &effect) const {
    Require(static_cast<bool>(effect), "held successor readback has no consumer");
    RunWithExactHeldSuccessor(query, successor, [&] {
      const auto status = QueryStatusLocked(query);
      Require(status.matches(query) && status.state == core::ConfigurationFenceState::kHeld &&
                  status.admission_fenced, "held successor readback changed under interlock");
      effect(status);
    });
  }

  [[nodiscard]] core::ConfigurationFenceReleasedDTO
  Release(const core::ConfigurationFenceReleaseDTO &request) {
    std::lock_guard lock(mutex_);
    Require(request.valid(), "fence release request is invalid");
    const auto &evidence = request.reconciliation_evidence;
    const auto row = ReadByFence(evidence.configuration_fence_id,
                                 evidence.configuration_fence_generation);
    Require(row.has_value(),
            "released fence is absent from the durable ledger");
    Require(row->owner_kind == DurableFenceOwnerKind::kApply,
            "Apply release names a route-configuration fence");
    if (row->state == 1) {
      Require(row->release_request_bytes.has_value() &&
                  row->release_receipt_bytes.has_value() &&
                  *row->release_request_bytes == EncodeRequest(request),
              "exact release replay conflicts with the durable receipt");
      return DecodeResponse<core::ConfigurationFenceReleasedDTO>(
          *row->release_receipt_bytes);
    }
    const core::ConfigurationFenceStatusQueryDTO query{
        evidence.owner_transaction, evidence.configuration_fence_id,
        evidence.configuration_fence_generation, evidence.reconciled_scope,
        request.manager_runtime_epoch};
    const auto status = QueryStatusLocked(query);
    Require(
        status.state == core::ConfigurationFenceState::kHeld &&
            status.reconciliation_evidence == evidence &&
            status.reconciliation_evidence.affected_queued_operation_count ==
                0U &&
            status.reconciliation_evidence.affected_active_operation_count ==
                0U,
        "fence release evidence is stale or affected work remains");
    core::ConfigurationFenceReleasedDTO released{
        request, evidence.execution_host_epoch, config_.now_ns(),
        core::RequestReplayStatusDTO::kAcceptedNew,
        Digest("manyforge.kernel-fence-release-receipt.v1",
               EncodeRequest(request),
               evidence.execution_host_epoch.durable_stop_fence_ledger_digest
                   .value())};
    Require(released.valid(), "constructed fence release receipt is invalid");
    const auto request_bytes = EncodeRequest(request);
    const auto receipt_bytes = EncodeResponse(released);
    Transaction transaction(database_);
    Statement update(
        database_,
        "UPDATE kernel_apply_fences SET state=1,release_request_blob=?,"
        "release_receipt_blob=? WHERE fence_id=? AND fence_generation=? AND "
        "state=0");
    update.Blob(1, request_bytes);
    update.Blob(2, receipt_bytes);
    update.Text(3, evidence.configuration_fence_id.value());
    update.Integer(4, evidence.configuration_fence_generation.value());
    Require(update.Step() == SQLITE_DONE && sqlite3_changes(database_) == 1,
            "durable fence release compare-and-swap failed");
    transaction.Commit();
    admission_gate_.ReleaseFence(evidence.configuration_fence_id,
                                 evidence.reconciled_scope);
    return released;
  }

  [[nodiscard]] core::RouteConfigurationFenceAdmissionSnapshotDTO
  ObserveRouteConfigurationFenceAdmission(
      const core::RouteConfigurationFenceAdmissionQueryDTO &query) const {
    std::lock_guard lock(mutex_);
    return ObserveRouteConfigurationFenceAdmissionLocked(query);
  }

  [[nodiscard]] core::RouteConfigurationFenceAcceptedDTO
  AcquireRouteConfigurationFence(
      const core::RouteConfigurationFenceIntentDTO &intent) {
    std::lock_guard lock(mutex_);
    Require(intent.locally_valid(),
            "route-configuration fence intent is invalid");
    const auto intent_bytes =
        core::detail::EncodeRouteFenceIntentRecord(intent);
    if (const auto existing =
            ReadByRequest(intent.owner_request_context.request_owner_epoch,
                          intent.owner_request_context.request_id)) {
      Require(existing->owner_kind == DurableFenceOwnerKind::kRoute &&
                  existing->intent_bytes == intent_bytes,
              "request ID is bound to another configuration-fence outcome");
      auto accepted = core::detail::DecodeRouteFenceAcceptedRecord(
          existing->accepted_bytes);
      accepted.replay_status = core::RequestReplayStatusDTO::kReplayedExact;
      Require(accepted.matches(intent),
              "durable route-fence replay contradicts its intent");
      return accepted;
    }
    Require(
        ReadSingleInteger(database_, "SELECT COUNT(*) FROM kernel_apply_fences",
                          "durable fence record count") < kMaximumFenceRecords,
        "durable fence ledger has reached its fixed record bound");

    const core::RouteConfigurationFenceAdmissionQueryDTO query{
        intent.route_selection_transaction_id,
        intent.route_selection_transaction_generation,
        intent.scope,
        intent.expected_retained_bundle_id,
        intent.expected_graph_revision,
        intent.expected_active_control_configuration};
    const auto snapshot = ObserveRouteConfigurationFenceAdmissionLocked(query);
    Require(intent.matches(snapshot),
            "route-fence admission changed before acquisition");

    const auto predecessor_generation = ReadFenceGeneration();
    Require(
        predecessor_generation == intent.expected_fence_generation.value() &&
            predecessor_generation < std::numeric_limits<std::uint64_t>::max(),
        "configuration-fence generation changed before route acquisition");
    const auto next_generation = predecessor_generation + 1U;
    core::RouteConfigurationFenceAcceptedDTO accepted{
        intent.owner_request_context.request_id,
        core::RequestReplayStatusDTO::kAcceptedNew,
        intent.route_selection_transaction_id,
        intent.route_selection_transaction_generation,
        core::ConfigurationFenceId(FenceIdFor(intent, next_generation)),
        core::ConfigurationFenceGeneration(next_generation),
        intent.scope,
        intent.candidate_active_control_configuration_id,
        intent.candidate_active_control_configuration_generation,
        config_.now_ns(),
        Digest("manyforge.kernel-route-fence-acceptance.seed.v1")};
    accepted.acceptance_digest =
        core::ContentDigest(content_identity::sha256_digest(
            core::detail::EncodeRouteFenceAcceptedRecord(accepted)));
    Require(accepted.matches(intent),
            "constructed route-fence acceptance is invalid");

    admission_gate_.AcquireRouteFenceDurably(
        accepted.configuration_fence_id, accepted.captured_scope,
        intent.expected_operation_resource_generation,
        snapshot.current_active_control_configuration, [&] {
          Transaction transaction(database_);
          Statement insert(
              database_,
              "INSERT INTO kernel_apply_fences ("
              "owner_kind,owner_id,owner_generation,request_id,"
              "request_owner_epoch,intent_blob,accepted_blob,fence_id,"
              "fence_generation,state,release_request_blob,"
              "release_receipt_blob) VALUES (?,?,?,?,?,?,?,?,?,0,NULL,NULL)");
          insert.Text(1, ToString(DurableFenceOwnerKind::kRoute));
          insert.Text(2, intent.route_selection_transaction_id.value());
          insert.Integer(3,
                         intent.route_selection_transaction_generation.value());
          insert.Text(4, intent.owner_request_context.request_id.value());
          insert.Text(5,
                      intent.owner_request_context.request_owner_epoch.value());
          insert.Blob(6, intent_bytes);
          insert.Blob(7,
                      core::detail::EncodeRouteFenceAcceptedRecord(accepted));
          insert.Text(8, accepted.configuration_fence_id.value());
          insert.Integer(9, accepted.configuration_fence_generation.value());
          Require(insert.Step() == SQLITE_DONE,
                  "durable route-fence insertion returned a row");
          Statement update(
              database_,
              "UPDATE kernel_apply_fence_meta SET fence_generation=? WHERE "
              "id=1 AND fence_generation=?");
          update.Integer(1, next_generation);
          update.Integer(2, predecessor_generation);
          Require(update.Step() == SQLITE_DONE &&
                      sqlite3_changes(database_) == 1,
                  "durable route-fence generation compare-and-swap failed");
          transaction.Commit();
        });
    return accepted;
  }

  [[nodiscard]]
  core::RouteConfigurationFenceAcquireAttemptReconciliationDTO
  ReconcileRouteConfigurationFenceAcquireAttempt(
      const core::RouteConfigurationFenceIntentDTO &intent) const {
    std::lock_guard lock(mutex_);
    Require(intent.locally_valid(),
            "route-fence Acquire reconciliation intent is invalid");
    const auto intent_bytes =
        core::detail::EncodeRouteFenceIntentRecord(intent);
    const auto existing =
        ReadByRequest(intent.owner_request_context.request_owner_epoch,
                      intent.owner_request_context.request_id);
    if (!existing.has_value()) {
      return {
          intent,
          core::RouteConfigurationFenceAcquireAttemptDispositionDTO::kNotSeen,
          std::nullopt, std::nullopt, std::nullopt};
    }
    if (existing->owner_kind != DurableFenceOwnerKind::kRoute ||
        existing->intent_bytes != intent_bytes) {
      return {intent,
              core::RouteConfigurationFenceAcquireAttemptDispositionDTO::
                  kConflicting,
              std::nullopt, std::nullopt,
              core::ErrorDTO{"route_configuration_fence_request_conflict",
                             "the durable request identity names another fence",
                             {}}};
    }
    const auto accepted =
        core::detail::DecodeRouteFenceAcceptedRecord(existing->accepted_bytes);
    const core::RouteConfigurationFenceStatusQueryDTO query{
        accepted.route_selection_transaction_id,
        accepted.route_selection_transaction_generation,
        accepted.configuration_fence_id,
        accepted.configuration_fence_generation, accepted.captured_scope};
    const auto status = ObserveRouteConfigurationFenceStatusLocked(query);
    if (existing->state == 0) {
      core::RouteConfigurationFenceAcquireAttemptReconciliationDTO result{
          intent,
          core::RouteConfigurationFenceAcquireAttemptDispositionDTO::
              kAcceptedExact,
          accepted, status, std::nullopt};
      Require(result.locally_valid(),
              "constructed exact route-fence reconciliation is invalid");
      return result;
    }
    core::RouteConfigurationFenceAcquireAttemptReconciliationDTO result{
        intent,
        core::RouteConfigurationFenceAcquireAttemptDispositionDTO::kConflicting,
        std::nullopt, status,
        core::ErrorDTO{
            "route_configuration_fence_already_released",
            "the exact Acquire was accepted and its fence is already released",
            {}}};
    Require(result.locally_valid(),
            "constructed released Acquire reconciliation is invalid");
    return result;
  }

  [[nodiscard]] core::RouteConfigurationFenceStatusDTO
  ObserveRouteConfigurationFenceStatus(
      const core::RouteConfigurationFenceStatusQueryDTO &query) const {
    std::lock_guard lock(mutex_);
    return ObserveRouteConfigurationFenceStatusLocked(query);
  }

  [[nodiscard]] core::RouteConfigurationFenceReleasedDTO
  ReleaseRouteConfigurationFence(
      const core::RouteConfigurationFenceReleaseRequestDTO &request) {
    std::lock_guard lock(mutex_);
    Require(request.locally_valid(), "route-fence release request is invalid");
    const auto request_bytes =
        core::detail::EncodeRouteFenceReleaseRequestRecord(request);
    const auto row =
        ReadByFence(request.accepted_fence.configuration_fence_id,
                    request.accepted_fence.configuration_fence_generation);
    Require(row.has_value() && row->owner_kind == DurableFenceOwnerKind::kRoute,
            "route-fence release identity is absent or foreign");
    if (row->state == 1) {
      Require(row->release_request_bytes == request_bytes &&
                  row->release_receipt_bytes.has_value(),
              "exact route-fence release replay conflicts with its receipt");
      auto released = core::detail::DecodeRouteFenceReleasedRecord(
          *row->release_receipt_bytes);
      released.replay_status = core::RequestReplayStatusDTO::kReplayedExact;
      return released;
    }
    const auto intent =
        core::detail::DecodeRouteFenceIntentRecord(row->intent_bytes);
    const auto accepted =
        core::detail::DecodeRouteFenceAcceptedRecord(row->accepted_bytes);
    Require(accepted == request.accepted_fence && accepted.matches(intent),
            "route-fence release conflicts with its durable acceptance");
    const core::RouteConfigurationFenceStatusQueryDTO query{
        accepted.route_selection_transaction_id,
        accepted.route_selection_transaction_generation,
        accepted.configuration_fence_id,
        accepted.configuration_fence_generation, accepted.captured_scope};
    const auto held = ObserveRouteConfigurationFenceStatusLocked(query);
    Require(held.state == core::RouteConfigurationFenceStateDTO::kHeld &&
                held.evidence == request.held_evidence,
            "route-fence release evidence is stale or no longer held");

    core::RouteConfigurationFenceReleasedDTO released{
        request,
        core::RequestReplayStatusDTO::kAcceptedNew,
        config_.now_ns(),
        accepted.configuration_fence_id,
        accepted.configuration_fence_generation,
        Digest("manyforge.kernel-route-fence-released-scope.v1",
               core::detail::EncodeRouteFenceAcceptedRecord(accepted)),
        Digest("manyforge.kernel-route-fence-release-receipt.seed.v1")};
    released.receipt_digest =
        core::ContentDigest(content_identity::sha256_digest(
            core::detail::EncodeRouteFenceReleasedRecord(released)));
    Require(released.locally_valid(),
            "constructed route-fence release receipt is invalid");
    const auto receipt_bytes =
        core::detail::EncodeRouteFenceReleasedRecord(released);
    Transaction transaction(database_);
    Statement update(
        database_,
        "UPDATE kernel_apply_fences SET state=1,release_request_blob=?,"
        "release_receipt_blob=? WHERE fence_id=? AND fence_generation=? AND "
        "owner_kind='route' AND state=0");
    update.Blob(1, request_bytes);
    update.Blob(2, receipt_bytes);
    update.Text(3, accepted.configuration_fence_id.value());
    update.Integer(4, accepted.configuration_fence_generation.value());
    Require(update.Step() == SQLITE_DONE && sqlite3_changes(database_) == 1,
            "durable route-fence release compare-and-swap failed");
    transaction.Commit();
    admission_gate_.ReleaseFence(accepted.configuration_fence_id,
                                 accepted.captured_scope);
    return released;
  }

  [[nodiscard]]
  core::RouteConfigurationFenceReleaseAttemptReconciliationDTO
  ReconcileRouteConfigurationFenceReleaseAttempt(
      const core::RouteConfigurationFenceReleaseRequestDTO &request) const {
    std::lock_guard lock(mutex_);
    Require(request.locally_valid(),
            "route-fence Release reconciliation request is invalid");
    const auto row =
        ReadByFence(request.accepted_fence.configuration_fence_id,
                    request.accepted_fence.configuration_fence_generation);
    if (!row.has_value() || row->owner_kind != DurableFenceOwnerKind::kRoute) {
      return {request,
              core::RouteConfigurationFenceReleaseAttemptDispositionDTO::
                  kConflicting,
              std::nullopt, std::nullopt,
              core::ErrorDTO{"route_configuration_fence_release_conflict",
                             "the release names an absent or foreign fence",
                             {}}};
    }
    const auto encoded =
        core::detail::EncodeRouteFenceReleaseRequestRecord(request);
    const auto accepted =
        core::detail::DecodeRouteFenceAcceptedRecord(row->accepted_bytes);
    const core::RouteConfigurationFenceStatusQueryDTO query{
        accepted.route_selection_transaction_id,
        accepted.route_selection_transaction_generation,
        accepted.configuration_fence_id,
        accepted.configuration_fence_generation, accepted.captured_scope};
    const auto status = ObserveRouteConfigurationFenceStatusLocked(query);
    if (row->state == 1 && row->release_request_bytes == encoded &&
        row->release_receipt_bytes.has_value()) {
      const auto receipt = core::detail::DecodeRouteFenceReleasedRecord(
          *row->release_receipt_bytes);
      core::RouteConfigurationFenceReleaseAttemptReconciliationDTO result{
          request,
          core::RouteConfigurationFenceReleaseAttemptDispositionDTO::
              kReleasedExact,
          receipt, status, std::nullopt};
      Require(result.locally_valid(),
              "constructed exact route release reconciliation is invalid");
      return result;
    }
    if (row->state == 0 && status.evidence == request.held_evidence) {
      core::RouteConfigurationFenceReleaseAttemptReconciliationDTO result{
          request,
          core::RouteConfigurationFenceReleaseAttemptDispositionDTO::
              kNotReleasedHeld,
          std::nullopt, status, std::nullopt};
      Require(result.locally_valid(),
              "constructed held route release reconciliation is invalid");
      return result;
    }
    core::RouteConfigurationFenceReleaseAttemptReconciliationDTO result{
        request,
        core::RouteConfigurationFenceReleaseAttemptDispositionDTO::kConflicting,
        std::nullopt, status,
        core::ErrorDTO{
            "route_configuration_fence_release_conflict",
            "the durable release outcome conflicts with the exact request",
            {}}};
    Require(result.locally_valid(),
            "constructed conflicting route release reconciliation is invalid");
    return result;
  }

  [[nodiscard]] core::RouteConfigurationFenceAdmissionSnapshotDTO
  ObserveRouteConfigurationFenceAdmissionLocked(
      const core::RouteConfigurationFenceAdmissionQueryDTO &query) const {
    Require(query.locally_valid(), "route-fence admission query is invalid");
    const auto active = ReadActivePublication();
    Require(active.receipt.request.candidate_retained_bundle_id ==
                    query.expected_retained_bundle_id &&
                active.receipt.request.candidate_graph_revision ==
                    query.expected_graph_revision,
            "active publication differs from the route-fence expectation");
    const auto observation = admission_gate_.Observe(query.scope);
    const auto current = SelectExpectedConfiguration(
        observation.route_published_active_control_configurations, query.scope);
    Require(current == query.expected_active_control_configuration,
            "active control configuration differs from the route expectation");
    core::RouteConfigurationFenceAdmissionSnapshotDTO snapshot{
        query,
        ExecutionHostEpoch(active.receipt),
        active.receipt.request.candidate_projection_revision,
        active.receipt.publication_generation,
        active.receipt.durable_public_pointer_content_digest,
        observation.operation_resource_generation,
        core::ConfigurationFenceGeneration(ReadFenceGeneration()),
        current,
        observation.affected_queued_operation_count,
        observation.affected_active_operation_count,
        Digest("manyforge.kernel-route-fence-admission-snapshot.seed.v1")};
    snapshot.snapshot_digest =
        core::ContentDigest(content_identity::sha256_digest(
            core::detail::EncodeRouteFenceAdmissionSnapshotRecord(snapshot)));
    Require(snapshot.locally_valid(),
            "constructed route-fence admission snapshot is invalid");
    return snapshot;
  }

  [[nodiscard]] core::RouteConfigurationFenceStatusDTO
  ObserveRouteConfigurationFenceStatusLocked(
      const core::RouteConfigurationFenceStatusQueryDTO &query) const {
    Require(query.locally_valid(), "route-fence status query is invalid");
    const auto row = ReadByFence(query.configuration_fence_id,
                                 query.configuration_fence_generation);
    Require(row.has_value() && row->owner_kind == DurableFenceOwnerKind::kRoute,
            "route-fence status identity is absent or foreign");
    const auto intent =
        core::detail::DecodeRouteFenceIntentRecord(row->intent_bytes);
    const auto accepted =
        core::detail::DecodeRouteFenceAcceptedRecord(row->accepted_bytes);
    Require(accepted.matches(intent) &&
                accepted.route_selection_transaction_id ==
                    query.route_selection_transaction_id &&
                accepted.route_selection_transaction_generation ==
                    query.route_selection_transaction_generation &&
                accepted.configuration_fence_id ==
                    query.configuration_fence_id &&
                accepted.configuration_fence_generation ==
                    query.configuration_fence_generation &&
                accepted.captured_scope == query.expected_scope,
            "route-fence status query conflicts with the durable fence");
    if (row->state == 1) {
      Require(row->release_receipt_bytes.has_value(),
              "released route fence lacks its durable receipt");
      const auto released = core::detail::DecodeRouteFenceReleasedRecord(
          *row->release_receipt_bytes);
      core::RouteConfigurationFenceStatusDTO status{
          query,
          released.request.held_evidence,
          core::RouteConfigurationFenceStateDTO::kReleased,
          false,
          released,
          {},
          Digest("manyforge.kernel-route-fence-status.seed.v1"),
          std::nullopt};
      status.result_digest =
          core::ContentDigest(content_identity::sha256_digest(
              core::detail::EncodeRouteFenceStatusRecord(status)));
      Require(status.locally_valid(),
              "constructed released route-fence status is invalid");
      return status;
    }

    const auto active = ReadActivePublication();
    const auto observation = admission_gate_.Observe(query.expected_scope);
    const auto current = SelectExpectedConfiguration(
        observation.route_published_active_control_configurations,
        query.expected_scope);
    core::RouteConfigurationFenceReconciliationEvidenceDTO evidence{
        ExecutionHostEpoch(active.receipt),
        query.route_selection_transaction_id,
        query.route_selection_transaction_generation,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        observation.operation_resource_generation,
        active.receipt.request.candidate_retained_bundle_id,
        active.receipt.request.candidate_graph_revision,
        active.receipt.request.candidate_projection_revision,
        active.receipt.publication_generation,
        active.receipt.durable_public_pointer_content_digest,
        current,
        observation.affected_queued_operation_count,
        observation.affected_active_operation_count,
        Digest("manyforge.kernel-route-fence-evidence.seed.v1")};
    evidence.evidence_digest =
        core::ContentDigest(content_identity::sha256_digest(
            core::detail::EncodeRouteFenceEvidenceRecord(evidence)));
    Require(evidence.locally_valid(),
            "constructed route-fence reconciliation evidence is invalid");
    core::RouteConfigurationFenceStatusDTO status{
        query,
        evidence,
        core::RouteConfigurationFenceStateDTO::kHeld,
        true,
        std::nullopt,
        {},
        Digest("manyforge.kernel-route-fence-status.seed.v1"),
        std::nullopt};
    status.result_digest = core::ContentDigest(content_identity::sha256_digest(
        core::detail::EncodeRouteFenceStatusRecord(status)));
    Require(status.locally_valid(),
            "constructed held route-fence status is invalid");
    return status;
  }

  void ValidateSelectedClaimLocked(
      const core::SelectedBringUpConfigurationFenceIntentDTO &intent) const {
    Require(intent.valid() &&
                static_cast<bool>(config_.selected_claim_recovery_receipt),
            "selected fence has no current Kernel claim lookup");
    const auto &claim = intent.selected_claim;
    const auto receipt = config_.selected_claim_recovery_receipt(
        {claim.intent, claim, std::nullopt});
    Require(receipt.ok(), "selected admission claim lookup failed");
    const auto &value = receipt.value();
    Require(core::ValidateCellAdmissionRecoveryReceipt(value).ok() &&
                value.status ==
                    core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
                value.matched_claim == claim &&
                std::find(value.held_claims.begin(), value.held_claims.end(),
                          claim) != value.held_claims.end(),
            "selected admission claim is absent, changed or no longer held");
  }

  [[nodiscard]] core::ConfigurationFenceAcceptedDTO AcquireSelectedBringUpFence(
      const core::SelectedBringUpConfigurationFenceIntentDTO &intent) {
    std::lock_guard lock(mutex_);
    std::lock_guard admission_lock(*admission_gate_.shared_admission_mutex());
    Require(intent.valid(), "selected fence intent is invalid");
    const auto bytes = EncodeRequest(intent);
    if (const auto row =
            ReadByRequest(intent.owner_request_context.request_owner_epoch,
                          intent.owner_request_context.request_id)) {
      Require(row->owner_kind == DurableFenceOwnerKind::kSelectedBringUp &&
                  row->intent_bytes == bytes && row->state == 0,
              "selected request names another or released fence outcome");
      auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
          row->accepted_bytes);
      Require(intent.matches(accepted),
              "selected durable acceptance contradicts intent");
      ValidateSelectedClaimLocked(intent);
      accepted.replay_status = core::RequestReplayStatusDTO::kReplayedExact;
      return accepted;
    }
    ValidateSelectedClaimLocked(intent);
    Require(
        ReadSingleInteger(database_, "SELECT COUNT(*) FROM kernel_apply_fences",
                          "durable fence record count") < kMaximumFenceRecords,
        "durable fence ledger has reached its fixed record bound");
    const auto active = ReadActivePublication();
    Require(ValidateProviderConfigurations(intent.selected_scope, false,
                                           *active.bundle) ==
                intent.expected_admission.provider_lineages,
            "selected provider predecessor lineage changed before fence "
            "acquisition");
    const auto &expected = intent.expected_admission;
    Require(
        active.receipt.publication_generation ==
                intent.expected_publication_generation &&
            active.receipt.durable_public_pointer_content_digest ==
                expected.active_public_pointer_digest &&
            active.receipt.request.candidate_projection_revision ==
                expected.active_projection_revision &&
            expected.execution_host_epoch.runtime_unit_id ==
                config_.runtime_unit_id &&
            expected.execution_host_epoch.runtime_epoch ==
                config_.runtime_epoch &&
            expected.execution_host_epoch.request_owner_epoch ==
                config_.request_owner_epoch &&
            expected.execution_host_epoch.durable_stop_fence_ledger_digest ==
                LedgerDigest(),
        "selected predecessor publication or execution owner changed");
    const auto observation = admission_gate_.Observe(intent.selected_scope);
    Require(observation.operation_resource_generation ==
                    expected.operation_resource_generation &&
                observation.affected_queued_operation_count == 0U &&
                observation.affected_active_operation_count == 0U &&
                SelectConfigurations(observation.active_control_configurations,
                                     intent.selected_scope, false) ==
                    expected.active_control_configurations,
            "selected operations or predecessor configurations changed");
    const auto previous = ReadFenceGeneration();
    Require(previous == expected.configuration_fence_generation.value() &&
                previous < std::numeric_limits<std::uint64_t>::max(),
            "selected fence generation changed or exhausted");
    const auto next = previous + 1U;
    const auto hash = content_identity::sha256_digest(bytes);
    core::ConfigurationFenceAcceptedDTO accepted{
        core::ConfigurationFenceId("configuration-fence-" +
                                   std::to_string(next) + "-" +
                                   hash.substr(hash.size() - 16U)),
        core::ConfigurationFenceGeneration(next), intent.selected_scope,
        config_.now_ns(), core::RequestReplayStatusDTO::kAcceptedNew};
    Require(intent.matches(accepted),
            "constructed selected acceptance is invalid");
    admission_gate_.AcquireFenceDurably(
        accepted.configuration_fence_id, accepted.captured_scope,
        expected.operation_resource_generation,
        expected.active_control_configurations, [&] {
          Transaction transaction(database_);
          Statement insert(
              database_,
              "INSERT INTO kernel_apply_fences "
              "(owner_kind,owner_id,owner_generation,request_id,request_owner_"
              "epoch,intent_blob,accepted_blob,fence_id,fence_generation,state,"
              "release_request_blob,release_receipt_blob) VALUES "
              "(?,?,?,?,?,?,?,?,?,0,NULL,NULL)");
          insert.Text(1, ToString(DurableFenceOwnerKind::kSelectedBringUp));
          insert.Text(2, intent.lifecycle_transaction_id.value());
          insert.Integer(3, intent.lifecycle_transaction_generation.value());
          insert.Text(4, intent.owner_request_context.request_id.value());
          insert.Text(5,
                      intent.owner_request_context.request_owner_epoch.value());
          insert.Blob(6, bytes);
          insert.Blob(7, EncodeResponse(accepted));
          insert.Text(8, accepted.configuration_fence_id.value());
          insert.Integer(9, next);
          Require(insert.Step() == SQLITE_DONE,
                  "selected fence insertion returned a row");
          Statement update(
              database_,
              "UPDATE kernel_apply_fence_meta SET fence_generation=? WHERE "
              "id=1 AND fence_generation=?");
          update.Integer(1, next);
          update.Integer(2, previous);
          Require(update.Step() == SQLITE_DONE &&
                      sqlite3_changes(database_) == 1,
                  "selected fence generation compare-and-swap failed");
          transaction.Commit();
        });
    return accepted;
  }

  [[nodiscard]] core::SelectedBringUpConfigurationFenceReleasedDTO
  ReleaseSelectedBringUpFence(
      const core::SelectedBringUpConfigurationFenceReleaseDTO &request) {
    std::lock_guard lock(mutex_);
    std::lock_guard admission_lock(*admission_gate_.shared_admission_mutex());
    Require(request.valid(), "fence release request is invalid");
    const auto &evidence = request.reconciliation_evidence;
    const auto row = ReadByFence(evidence.configuration_fence_id,
                                 evidence.configuration_fence_generation);
    Require(row.has_value(),
            "released fence is absent from the durable ledger");
    Require(row->owner_kind == DurableFenceOwnerKind::kSelectedBringUp,
            "selected release names a foreign fence");
    if (row->state == 1) {
      Require(row->release_request_bytes.has_value() &&
                  row->release_receipt_bytes.has_value() &&
                  *row->release_request_bytes == EncodeRequest(request),
              "exact release replay conflicts with the durable receipt");
      Require(
          !admission_gate_.exact_fence_is_held(evidence.configuration_fence_id,
                                               evidence.reconciled_scope),
          "selected release is durable but admission remains held; preserve "
          "exclusion and reconcile the exact owner before proceeding");
      return DecodeResponse<core::SelectedBringUpConfigurationFenceReleasedDTO>(
          *row->release_receipt_bytes);
    }
    ValidateSelectedClaimLocked(
        DecodeRequest<core::SelectedBringUpConfigurationFenceIntentDTO>(
            row->intent_bytes));
    const core::SelectedBringUpConfigurationFenceStatusQueryDTO query{
        evidence.owner_transaction, evidence.configuration_fence_id,
        evidence.configuration_fence_generation, evidence.reconciled_scope,
        request.manager_runtime_epoch};
    const auto status = SelectedStatusLocked(query);
    const auto intent =
        DecodeRequest<core::SelectedBringUpConfigurationFenceIntentDTO>(
            row->intent_bytes);
    Require(status.reconciliation_evidence.active_projection_revision ==
                intent.candidate_projection_revision,
            "selected successor projection has not been installed");
    Require(
        status.state == core::ConfigurationFenceState::kHeld &&
            status.reconciliation_evidence == evidence &&
            status.reconciliation_evidence.affected_queued_operation_count ==
                0U &&
            status.reconciliation_evidence.affected_active_operation_count ==
                0U,
        "fence release evidence is stale or affected work remains");
    core::SelectedBringUpConfigurationFenceReleasedDTO released{
        request, evidence.execution_host_epoch, config_.now_ns(),
        core::RequestReplayStatusDTO::kAcceptedNew,
        Digest("manyforge.kernel-selected-fence-release-receipt.v1",
               EncodeRequest(request),
               evidence.execution_host_epoch.durable_stop_fence_ledger_digest
                   .value())};
    Require(released.valid(), "constructed fence release receipt is invalid");
    const auto request_bytes = EncodeRequest(request);
    const auto receipt_bytes = EncodeResponse(released);
    admission_gate_.ReleaseFenceDurably(
        evidence.configuration_fence_id, evidence.reconciled_scope, [&] {
          Transaction transaction(database_);
          Statement update(
              database_,
              "UPDATE kernel_apply_fences SET state=1,release_request_blob=?,"
              "release_receipt_blob=? WHERE fence_id=? AND fence_generation=? "
              "AND "
              "state=0");
          update.Blob(1, request_bytes);
          update.Blob(2, receipt_bytes);
          update.Text(3, evidence.configuration_fence_id.value());
          update.Integer(4, evidence.configuration_fence_generation.value());
          Require(update.Step() == SQLITE_DONE &&
                      sqlite3_changes(database_) == 1,
                  "durable fence release compare-and-swap failed");
          transaction.Commit();
        });
    return released;
  }

  [[nodiscard]] core::SelectedBringUpConfigurationFenceStatusDTO
  SelectedStatusLocked(
      const core::SelectedBringUpConfigurationFenceStatusQueryDTO &query)
      const {
    Require(query.valid(), "fence status query is invalid");
    std::unique_lock admission_lock(*admission_gate_.shared_admission_mutex(),
                                    std::defer_lock);
    if (HasProviderProof(query.expected_scope))
      admission_lock.lock();
    const auto row = ReadByFence(query.configuration_fence_id,
                                 query.configuration_fence_generation);
    Require(row.has_value(), "fence status identity is unknown");
    Require(row->owner_kind == DurableFenceOwnerKind::kSelectedBringUp,
            "selected status query names a foreign fence");
    const auto intent =
        DecodeRequest<core::SelectedBringUpConfigurationFenceIntentDTO>(
            row->intent_bytes);
    const auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
        row->accepted_bytes);
    Require(intent.owner() == query.owner_transaction &&
                accepted.configuration_fence_id ==
                    query.configuration_fence_id &&
                accepted.configuration_fence_generation ==
                    query.configuration_fence_generation &&
                accepted.captured_scope == query.expected_scope,
            "fence status query conflicts with the durable fence");
    if (row->state == 1) {
      Require(row->release_receipt_bytes.has_value(),
              "released fence lacks its durable receipt");
      const auto released =
          DecodeResponse<core::SelectedBringUpConfigurationFenceReleasedDTO>(
              *row->release_receipt_bytes);
      const bool gate_held = admission_gate_.exact_fence_is_held(
          query.configuration_fence_id, query.expected_scope);
      if (gate_held) {
        core::SelectedBringUpConfigurationFenceStatusDTO pending{
            query,
            released.request.reconciliation_evidence,
            core::ConfigurationFenceState::kReleasePending,
            true,
            std::nullopt,
            "preserve selected admission exclusion and obtain explicit owner "
            "reconciliation of the durable release and held gate",
            Digest("manyforge.kernel-selected-fence-release-pending.seed.v1"),
            core::ErrorDTO{"selected_bring_up_fence_release_uncertain",
                           "the release receipt is durable but its admission "
                           "gate remains held",
                           {}}};
        pending.result_digest = core::ContentDigest(
            content_identity::sha256_digest(EncodeResponse(pending)));
        Require(pending.presence_valid(),
                "constructed pending selected release status is invalid");
        return pending;
      }
      core::SelectedBringUpConfigurationFenceStatusDTO status{
          query,
          released.request.reconciliation_evidence,
          core::ConfigurationFenceState::kReleased,
          false,
          released,
          {},
          Digest("manyforge.kernel-selected-fence-status.v1",
                 EncodeRequest(query), released.release_receipt_digest.value()),
          std::nullopt};
      Require(status.presence_valid(),
              "constructed released fence status is invalid");
      return status;
    }

    Require(admission_gate_.exact_fence_is_held(query.configuration_fence_id,
                                                query.expected_scope),
            "selected durable fence is absent from the admission gate");
    const auto active = ReadActivePublication();
    ValidateProviderConfigurations(query.expected_scope, true, *active.bundle);
    const auto observation = admission_gate_.Observe(query.expected_scope);
    const auto configurations = SelectConfigurations(
        observation.active_control_configurations, query.expected_scope, true);
    const auto epoch = ExecutionHostEpoch(active.receipt);
    core::SelectedBringUpConfigurationFenceReconciliationEvidenceDTO evidence{
        epoch,
        query.owner_transaction,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        observation.operation_resource_generation,
        active.receipt.request.candidate_graph_revision,
        active.receipt.request.candidate_projection_revision,
        active.receipt.publication_generation,
        active.receipt.durable_public_pointer_content_digest,
        configurations,
        observation.affected_queued_operation_count,
        observation.affected_active_operation_count,
        Digest(
            "manyforge.kernel-selected-fence-reconciliation-evidence.seed.v1")};
    Require(evidence.valid(),
            "constructed fence reconciliation evidence is invalid");
    core::SelectedBringUpConfigurationFenceStatusDTO status{
        query,
        evidence,
        core::ConfigurationFenceState::kHeld,
        true,
        std::nullopt,
        {},
        Digest("manyforge.kernel-selected-fence-status.seed.v1"),
        std::nullopt};
    evidence.evidence_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(status)));
    status.reconciliation_evidence = evidence;
    status.result_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(status)));
    Require(status.presence_valid(),
            "constructed held fence status is invalid");
    return status;
  }

  [[nodiscard]] core::SelectedBringUpConfigurationFenceStatusDTO
  ObserveSelectedBringUpFenceStatus(
      const core::SelectedBringUpConfigurationFenceStatusQueryDTO &query)
      const {
    std::lock_guard lock(mutex_);
    return SelectedStatusLocked(query);
  }

  void RunWithExactHeldSelectedPredecessor(
      const core::SelectedBringUpConfigurationFenceStatusQueryDTO &query,
      const std::function<void()> &effect) const {
    std::unique_lock lock(mutex_);
    std::unique_lock admission_lock(*admission_gate_.shared_admission_mutex());
    Require(query.valid() && static_cast<bool>(effect),
            "selected fenced effect is invalid");
    const auto row = ReadByFence(query.configuration_fence_id,
                                 query.configuration_fence_generation);
    Require(row && row->owner_kind == DurableFenceOwnerKind::kSelectedBringUp &&
                row->state == 0,
            "selected effect names an absent, foreign or released fence");
    const auto intent =
        DecodeRequest<core::SelectedBringUpConfigurationFenceIntentDTO>(
            row->intent_bytes);
    const auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
        row->accepted_bytes);
    Require(intent.owner() == query.owner_transaction &&
                intent.matches(accepted) &&
                accepted.captured_scope == query.expected_scope,
            "selected effect conflicts with durable intent");
    Require(intent.expected_admission.execution_host_epoch.runtime_unit_id ==
                    config_.runtime_unit_id &&
                intent.expected_admission.execution_host_epoch.runtime_epoch ==
                    config_.runtime_epoch,
            "selected effect cannot reuse a predecessor from another execution "
            "occurrence");
    ValidateSelectedClaimLocked(intent);
    const auto active = ReadActivePublication();
    Require(ValidateProviderConfigurations(query.expected_scope, false,
                                           *active.bundle) ==
                intent.expected_admission.provider_lineages,
            "selected provider predecessor lineage changed before effect");
    Require(active.receipt.publication_generation ==
                    intent.expected_publication_generation &&
                active.receipt.durable_public_pointer_content_digest ==
                    intent.expected_admission.active_public_pointer_digest,
            "selected predecessor publication changed before effect");
    // The gate pins this exact fence across process retirement outside shared
    // locks, as the existing Apply interlock does. Release cannot pass the pin.
    admission_gate_.RunWithExactHeldFence(query.configuration_fence_id,
                                          query.expected_scope, [&] {
                                            admission_lock.unlock();
                                            lock.unlock();
                                            effect();
                                          });
  }

  [[nodiscard]] core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO
  ReconcileSelectedBringUpFenceAcquireAttempt(
      const core::SelectedBringUpFenceAcquireAttemptQueryDTO &query) const {
    std::lock_guard lock(mutex_);
    Require(query.valid(), "selected acquire reconciliation query is invalid");
    const auto &intent = query.persisted_intent;
    core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO result{
        ExecutionHostEpoch(ReadActivePublication().receipt),
        query,
        core::ApplyFenceAcquireAttemptReconcileState::kNotSeen,
        std::nullopt,
        false,
        {},
        Digest("manyforge.kernel-selected-fence-acquire-reconcile.seed.v1"),
        std::nullopt};
    const auto row =
        ReadByRequest(intent.owner_request_context.request_owner_epoch,
                      intent.owner_request_context.request_id);
    if (row) {
      if (row->owner_kind == DurableFenceOwnerKind::kSelectedBringUp &&
          row->intent_bytes == EncodeRequest(intent) && row->state == 0) {
        const auto accepted =
            DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
                row->accepted_bytes);
        Require(intent.matches(accepted),
                "selected acquire receipt contradicts intent");
        result.state =
            core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact;
        result.accepted_fence = accepted;
        result.admission_fenced = true;
      } else {
        result.state =
            core::ApplyFenceAcquireAttemptReconcileState::kConflicting;
        result.admission_fenced = true;
        result.required_compensation_or_operator_direction =
            "reconcile the exact selected fence outcome; do not repeat effects";
        result.error =
            core::ErrorDTO{"selected_bring_up_fence_request_conflict",
                           "request names another or released fence",
                           {}};
      }
    }
    if (!row && !admission_gate_
                     .current_fenced_resources(intent.selected_scope.resources)
                     .empty()) {
      result.state = core::ApplyFenceAcquireAttemptReconcileState::kUncertain;
      result.admission_fenced = true;
      result.required_compensation_or_operator_direction =
          "keep selected admission closed and reconcile the uncertain durable "
          "acquisition";
      result.error = core::ErrorDTO{"selected_bring_up_fence_acquire_uncertain",
                                    "the selected admission gate is held "
                                    "without an exact durable request receipt",
                                    {}};
    }
    result.result_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(result)));
    Require(result.presence_valid(),
            "constructed selected acquire reconciliation is invalid");
    return result;
  }

  [[nodiscard]] core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO
  ReconcileSelectedBringUpFenceReleaseAttempt(
      const core::SelectedBringUpFenceReleaseAttemptQueryDTO &query) const {
    std::lock_guard lock(mutex_);
    Require(query.valid(), "selected release reconciliation is invalid");
    const auto &evidence = query.persisted_request.reconciliation_evidence;
    core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO result{
        query, core::SelectedBringUpFenceReleaseAttemptState::kConflicting,
        std::nullopt, std::nullopt,
        core::ErrorDTO{"selected_bring_up_fence_release_conflict",
                       "release is absent or conflicts with the exact fence",
                       {}}};
    const auto row = ReadByFence(evidence.configuration_fence_id,
                                 evidence.configuration_fence_generation);
    if (row && row->owner_kind == DurableFenceOwnerKind::kSelectedBringUp) {
      result.observed_status = SelectedStatusLocked(
          {evidence.owner_transaction, evidence.configuration_fence_id,
           evidence.configuration_fence_generation, evidence.reconciled_scope,
           query.persisted_request.manager_runtime_epoch});
      if (row->state == 1 &&
          result.observed_status->state ==
              core::ConfigurationFenceState::kReleasePending) {
        result.state =
            core::SelectedBringUpFenceReleaseAttemptState::kUncertain;
        result.error =
            core::ErrorDTO{"selected_bring_up_fence_release_uncertain",
                           "the durable release and held admission gate "
                           "require explicit owner reconciliation",
                           {}};
      } else if (row->state == 1 &&
                 row->release_request_bytes ==
                     EncodeRequest(query.persisted_request) &&
                 row->release_receipt_bytes) {
        result.state =
            core::SelectedBringUpFenceReleaseAttemptState::kReleasedExact;
        result.release_receipt =
            DecodeResponse<core::SelectedBringUpConfigurationFenceReleasedDTO>(
                *row->release_receipt_bytes);
        result.error.reset();
      } else if (row->state == 0 &&
                 result.observed_status->reconciliation_evidence == evidence) {
        result.state =
            core::SelectedBringUpFenceReleaseAttemptState::kNotReleasedHeld;
        result.error.reset();
      }
    }
    Require(result.presence_valid(),
            "constructed selected release reconciliation is invalid");
    return result;
  }

  [[nodiscard]] static ManagedKernelApplyFenceOwnerConfig
  ValidateConfig(ManagedKernelApplyFenceOwnerConfig config) {
    config.ledger_path = ValidateLedgerPath(std::move(config.ledger_path));
    config.manager_state_root = ExistingPlainDirectory(
        std::move(config.manager_state_root), "Manager state root");
    config.retained_package_root = ExistingPlainDirectory(
        std::move(config.retained_package_root), "retained-package root");
    if (!config.candidate_retained_package_root.empty()) {
      config.candidate_retained_package_root = ExistingPlainDirectory(
          std::move(config.candidate_retained_package_root),
          "candidate retained-package root");
      RequireDisjointDirectories(config.retained_package_root,
                                 config.candidate_retained_package_root,
                                 "retained/activation roots");
    }
    Require(!config.cell_id.empty() && !config.runtime_unit_id.empty() &&
                !config.runtime_epoch.empty() &&
                !config.request_owner_epoch.empty() &&
                static_cast<bool>(config.now_ns),
            "owner construction identities or clock are invalid");
    return config;
  }

  void OpenDatabase() {
    Require(sqlite3_libversion_number() >= 3037000,
            "Kernel Apply-fence storage requires SQLite 3.37.0 or newer");
    try {
      static_cast<void>(persistence::PreflightExistingSqliteReadOnly(
          config_.ledger_path,
          {kMaximumDatabasePageImageBytes, kMaximumDatabasePageImageBytes},
          [&](sqlite3 *preflight) {
            ValidateExistingLedgerForPreflight(preflight, config_.cell_id);
          }));
    } catch (const persistence::SqliteReadOnlyPreflightError &error) {
      Fail("durable fence ledger '" + config_.ledger_path.string() +
           "' read-only preflight failed: " + std::string(error.what()));
    } catch (const std::runtime_error &error) {
      // Envelope/version validators propagate their refusal unchanged through
      // the read-only preflight callback; retain the actual ledger filename.
      Fail("durable fence ledger '" + config_.ledger_path.string() +
           "' read-only preflight failed: " + std::string(error.what()));
    }
    int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    if (sqlite3_open_v2(config_.ledger_path.c_str(), &database_, flags,
                        nullptr) != SQLITE_OK) {
      FailSqlite(database_, "open Kernel Apply-fence ledger");
    }
    if (sqlite3_extended_result_codes(database_, 1) != SQLITE_OK ||
        sqlite3_busy_timeout(database_, 2000) != SQLITE_OK) {
      FailSqlite(database_, "configure Kernel Apply-fence ledger");
    }
    Exec(database_, "PRAGMA foreign_keys=ON");
    Exec(database_, "PRAGMA trusted_schema=OFF");
  }

  void InitializeSchema() {
    const bool fresh = ValidateLedgerEnvelope(database_);
    Exec(database_, "PRAGMA journal_mode=WAL");
    Exec(database_, "PRAGMA synchronous=FULL");
    Exec(database_, "PRAGMA secure_delete=ON");
    const auto page_size =
        ReadSingleInteger(database_, "PRAGMA page_size", "SQLite page size");
    Require(page_size != 0U && kMaximumDatabasePageImageBytes % page_size == 0U,
            "SQLite page size cannot represent the fixed database bound");
    const auto maximum_page_count = kMaximumDatabasePageImageBytes / page_size;
    Exec(database_,
         "PRAGMA max_page_count=" + std::to_string(maximum_page_count));
    Require(ReadSingleInteger(database_, "PRAGMA max_page_count",
                              "SQLite maximum page count") ==
                maximum_page_count,
            "SQLite did not accept the fixed database bound");
    Exec(database_, "PRAGMA journal_size_limit=" +
                        std::to_string(kMaximumDatabasePageImageBytes));
    Require(ReadSingleInteger(database_, "PRAGMA journal_size_limit",
                              "SQLite journal-size limit") ==
                kMaximumDatabasePageImageBytes,
            "SQLite did not accept the fixed journal-size bound");
    Transaction transaction(database_);
    if (fresh) {
      Exec(database_,
           "CREATE TABLE kernel_apply_fence_meta ("
           "id INTEGER PRIMARY KEY CHECK(id=1),schema_id TEXT NOT NULL,"
           "schema_version INTEGER NOT NULL,cell_id TEXT NOT NULL,"
           "fence_generation INTEGER NOT NULL CHECK(fence_generation>=0),"
           "maximum_fence_records INTEGER NOT NULL CHECK("
           "maximum_fence_records>0),maximum_database_page_image_bytes "
           "INTEGER NOT NULL CHECK(maximum_database_page_image_bytes>0))");
      Exec(
          database_,
          "CREATE TABLE kernel_apply_fences ("
          "owner_kind TEXT NOT NULL CHECK(owner_kind IN "
          "('apply','route','selected_bring_up')),owner_id TEXT NOT NULL,"
          "owner_generation INTEGER NOT NULL CHECK("
          "owner_generation>0),request_id TEXT NOT NULL,request_owner_epoch "
          "TEXT NOT NULL,intent_blob BLOB NOT NULL,accepted_blob BLOB NOT NULL,"
          "fence_id TEXT NOT NULL,fence_generation INTEGER NOT NULL CHECK("
          "fence_generation>0),state INTEGER NOT NULL CHECK(state IN (0,1)),"
          "release_request_blob BLOB,release_receipt_blob BLOB,"
          "PRIMARY KEY(request_owner_epoch,request_id),"
          "UNIQUE(owner_kind,owner_id,owner_generation),UNIQUE(fence_id),"
          "UNIQUE(fence_generation),"
          "CHECK((state=0 AND release_request_blob IS "
          "NULL AND release_receipt_blob IS NULL) OR (state=1 AND "
          "release_request_blob IS NOT NULL AND release_receipt_blob IS NOT "
          "NULL)))");
    }
    Statement count(database_, "SELECT COUNT(*) FROM kernel_apply_fence_meta");
    Require(count.Step() == SQLITE_ROW, "metadata count is absent");
    const auto rows = count.ColumnInteger(0);
    Require(count.Step() == SQLITE_DONE, "metadata count has extra rows");
    if (rows == 0U) {
      Statement insert(
          database_,
          "INSERT INTO kernel_apply_fence_meta (id,schema_id,schema_version,"
          "cell_id,fence_generation,maximum_fence_records,"
          "maximum_database_page_image_bytes) VALUES (1,?,?,?,0,?,?)");
      insert.Text(1, kSchemaId);
      insert.Integer(2, kSchemaVersion);
      insert.Text(3, config_.cell_id.value());
      insert.Integer(4, kMaximumFenceRecords);
      insert.Integer(5, kMaximumDatabasePageImageBytes);
      Require(insert.Step() == SQLITE_DONE,
              "metadata insertion returned a row");
    } else {
      Require(rows == 1U, "metadata singleton contains extra rows");
      Statement metadata(database_, "SELECT schema_id,schema_version,cell_id,"
                                    "maximum_fence_records,"
                                    "maximum_database_page_image_bytes FROM "
                                    "kernel_apply_fence_meta WHERE id=1");
      Require(metadata.Step() == SQLITE_ROW &&
                  metadata.ColumnText(0) == kSchemaId &&
                  metadata.ColumnInteger(1) == kSchemaVersion &&
                  metadata.ColumnText(2) == config_.cell_id.value() &&
                  metadata.ColumnInteger(3) == kMaximumFenceRecords &&
                  metadata.ColumnInteger(4) == kMaximumDatabasePageImageBytes &&
                  metadata.Step() == SQLITE_DONE,
              "durable ledger metadata differs from this owner");
    }
    if (fresh) {
      Exec(database_,
           "PRAGMA application_id=" + std::to_string(kSqliteApplicationId));
      Exec(database_, "PRAGMA user_version=" + std::to_string(kSchemaVersion));
    }
    transaction.Commit();
    Statement integrity(database_, "PRAGMA quick_check");
    Require(integrity.Step() == SQLITE_ROW && integrity.ColumnText(0) == "ok" &&
                integrity.Step() == SQLITE_DONE,
            "durable ledger failed its SQLite integrity check");
    Require(
        ReadSingleInteger(database_, "SELECT COUNT(*) FROM kernel_apply_fences",
                          "durable fence record count") <= kMaximumFenceRecords,
        "durable ledger exceeds its fixed record bound");
  }

  void RestoreHeldFence() {
    Statement statement(database_,
                        "SELECT owner_kind,intent_blob,accepted_blob,"
                        "fence_id FROM "
                        "kernel_apply_fences WHERE state=0 ORDER "
                        "BY fence_generation");
    while (statement.Step() == SQLITE_ROW) {
      const auto owner_kind = statement.ColumnText(0);
      if (owner_kind == ToString(DurableFenceOwnerKind::kApply)) {
        const auto accepted =
            DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
                statement.ColumnBlob(2));
        admission_gate_.RestoreFence(accepted.configuration_fence_id,
                                     accepted.captured_scope);
      } else if (owner_kind == ToString(DurableFenceOwnerKind::kRoute)) {
        const auto accepted = core::detail::DecodeRouteFenceAcceptedRecord(
            statement.ColumnBlob(2));
        admission_gate_.RestoreFence(accepted.configuration_fence_id,
                                     accepted.captured_scope);
      } else if (owner_kind ==
                 ToString(DurableFenceOwnerKind::kSelectedBringUp)) {
        const auto intent =
            DecodeRequest<core::SelectedBringUpConfigurationFenceIntentDTO>(
                statement.ColumnBlob(1));
        const auto accepted =
            DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
                statement.ColumnBlob(2));
        Require(intent.matches(accepted),
                "selected durable restoration contradicts intent");
        admission_gate_.RestoreFence(accepted.configuration_fence_id,
                                     accepted.captured_scope);
      } else {
        Fail("durable fence has an unknown owner kind");
      }
    }
  }

  [[nodiscard]] std::uint64_t ReadFenceGeneration() const {
    Statement statement(
        database_,
        "SELECT fence_generation FROM kernel_apply_fence_meta WHERE id=1");
    Require(statement.Step() == SQLITE_ROW,
            "durable fence generation is absent");
    const auto generation = statement.ColumnInteger(0);
    Require(statement.Step() == SQLITE_DONE,
            "durable fence generation has extra rows");
    return generation;
  }

  [[nodiscard]] std::optional<DurableFenceRow>
  ReadByRequest(const core::RequestOwnerEpoch &owner_epoch,
                const core::RequestId &request_id) const {
    Statement statement(database_,
                        "SELECT "
                        "owner_kind,intent_blob,accepted_blob,"
                        "state,release_request_blob,"
                        "release_receipt_blob FROM kernel_apply_fences WHERE "
                        "request_owner_epoch=? AND request_id=?");
    statement.Text(1, owner_epoch.value());
    statement.Text(2, request_id.value());
    return ReadOptionalRow(statement);
  }

  [[nodiscard]] std::optional<DurableFenceRow>
  ReadByFence(const core::ConfigurationFenceId &fence_id,
              const core::ConfigurationFenceGeneration generation) const {
    Statement statement(
        database_,
        "SELECT "
        "owner_kind,intent_blob,accepted_blob,state,"
        "release_request_blob,"
        "release_receipt_blob FROM kernel_apply_fences WHERE fence_id=? AND "
        "fence_generation=?");
    statement.Text(1, fence_id.value());
    statement.Integer(2, generation.value());
    return ReadOptionalRow(statement);
  }

  [[nodiscard]] static std::optional<DurableFenceRow>
  ReadOptionalRow(Statement &statement) {
    if (statement.Step() == SQLITE_DONE) {
      return std::nullopt;
    }
    const auto owner_kind = statement.ColumnText(0);
    Require(owner_kind == ToString(DurableFenceOwnerKind::kApply) ||
                owner_kind == ToString(DurableFenceOwnerKind::kRoute) ||
                owner_kind == ToString(DurableFenceOwnerKind::kSelectedBringUp),
            "durable fence owner kind is invalid");
    const auto kind = owner_kind == ToString(DurableFenceOwnerKind::kApply)
                          ? DurableFenceOwnerKind::kApply
                      : owner_kind == ToString(DurableFenceOwnerKind::kRoute)
                          ? DurableFenceOwnerKind::kRoute
                          : DurableFenceOwnerKind::kSelectedBringUp;
    DurableFenceRow row{kind,
                        statement.ColumnBlob(1),
                        statement.ColumnBlob(2),
                        static_cast<int>(statement.ColumnInteger(3)),
                        statement.OptionalColumnBlob(4),
                        statement.OptionalColumnBlob(5)};
    Require(statement.Step() == SQLITE_DONE,
            "durable fence identity is not unique");
    return row;
  }

  [[nodiscard]] core::ContentDigest LedgerDigest() const {
    std::string bytes;
    AppendField(bytes, kSchemaId);
    AppendField(bytes, config_.cell_id.value());
    AppendField(bytes, std::to_string(ReadFenceGeneration()));
    Statement statement(
        database_, "SELECT "
                   "owner_kind,intent_blob,accepted_blob,"
                   "state,release_request_blob,"
                   "release_receipt_blob FROM kernel_apply_fences ORDER BY "
                   "fence_generation");
    while (statement.Step() == SQLITE_ROW) {
      AppendField(bytes, statement.ColumnText(0));
      AppendField(bytes, statement.ColumnBlob(1));
      AppendField(bytes, statement.ColumnBlob(2));
      AppendField(bytes, std::to_string(statement.ColumnInteger(3)));
      AppendField(bytes, statement.OptionalColumnBlob(4).value_or(""));
      AppendField(bytes, statement.OptionalColumnBlob(5).value_or(""));
    }
    return core::ContentDigest(content_identity::sha256_digest(bytes));
  }

  [[nodiscard]] static bool
  HasProviderProof(const core::ConfigurationFenceScopeDTO &scope) {
    return std::any_of(
        scope.domains.begin(), scope.domains.end(), [](const auto &domain) {
          return std::holds_alternative<
              core::ProviderConfigurationFenceDomainDTO>(domain.payload);
        });
  }

  void ValidateInstalledCandidateConfigurations(
      const core::ConfigurationFenceScopeDTO &scope,
      const deployment::RetainedBundle &bundle,
      const std::vector<core::ProviderConfigurationFenceLineageDTO> &lineages) const {
    std::size_t index = 0;
    for (const auto &domain : scope.domains) {
      const auto *sides = std::get_if<core::ProviderConfigurationFenceDomainDTO>(&domain.payload);
      if (!sides) continue;
      if (!sides->candidate_provider) {
        const auto sealed = std::find_if(bundle.value().cell_graph.control_domains.begin(),
            bundle.value().cell_graph.control_domains.end(), [&](const auto &row) {
              return row.control_domain_id == domain.control_domain_id;
            });
        Require(sealed == bundle.value().cell_graph.control_domains.end() || !sealed->provider(),
                "removed provider remains in the qualified candidate closure");
        Require(static_cast<bool>(config_.provider_configuration_source),
                "removed provider has no authoritative Kernel absence lookup");
        const auto current = config_.provider_configuration_source(domain.control_domain_id);
        Require(current.ok() && !current.value(),
                "removed provider remains installed in the current Kernel");
      } else {
        Require(index < lineages.size() && lineages[index].control_domain_id == domain.control_domain_id,
                "installed Apply candidate provider coverage is incomplete");
        ++index;
      }
    }
    Require(index == lineages.size() &&
                ValidateProviderConfigurations(scope, true, bundle) == lineages,
            "installed Apply candidate driver or lifecycle generation changed");
  }

  std::vector<core::ProviderConfigurationFenceLineageDTO>
  ValidateProviderConfigurations(
      const core::ConfigurationFenceScopeDTO &scope, bool candidate_side,
      const deployment::RetainedBundle &bundle) const {
    std::vector<core::ProviderConfigurationFenceLineageDTO> lineages;
    for (const auto &domain : scope.domains) {
      const auto *sides =
          std::get_if<core::ProviderConfigurationFenceDomainDTO>(
              &domain.payload);
      if (!sides)
        continue;
      const auto &proof = candidate_side ? sides->candidate_provider
                                         : sides->predecessor_provider;
      if (!proof)
        continue; // Mixed replacements may have a controller on this side.
      Require(static_cast<bool>(config_.provider_configuration_source),
              "provider fence has no current Kernel configuration lookup");
      const auto current =
          config_.provider_configuration_source(domain.control_domain_id);
      Require(current.ok() && current.value() &&
                  current.value()->locally_valid(),
              "provider fence has no exact installed Kernel configuration");
      const auto &row = *current.value();
      const auto &revision = candidate_side
                                 ? domain.candidate_control_domain_revision
                                 : domain.predecessor_control_domain_revision;
      const auto sealed = std::find_if(
          bundle.value().cell_graph.control_domains.begin(),
          bundle.value().cell_graph.control_domains.end(),
          [&](const auto &entry) {
            return entry.control_domain_id == domain.control_domain_id;
          });
      Require(revision && row.domain.locally_valid() &&
                  row.retained_bundle_id == bundle.retained_bundle_id() &&
                  row.graph_revision ==
                      bundle.value().cell_graph.graph_revision &&
                  row.domain.control_domain_id == domain.control_domain_id &&
                  row.domain.control_domain_revision == *revision &&
                  sealed != bundle.value().cell_graph.control_domains.end() &&
                  sealed->control_domain_revision == *revision &&
                  sealed->provider() &&
                  *sealed->provider() == row.domain.provider()->provider &&
                  row.domain.provider()->provider.controllable_component_id ==
                      proof->controllable_component_id &&
                  row.domain.provider()->provider.provider_instance_id ==
                      proof->provider_instance_id &&
                  row.domain.provider()->provider.signal_configuration_digest ==
                      proof->signal_configuration_digest &&
                  row.domain.resource_ids() == proof->output_resources,
              "provider fence configuration differs from its exact product and "
              "occurrence");
      lineages.push_back({domain.control_domain_id,
                          row.provider_occurrence.provider_runtime_epoch,
                          row.component_lifecycle_generation});
    }
    return lineages;
  }

  [[nodiscard]] ActivePublication ReadActivePublication() const {
    const auto integrity = publication_store_.VerifyIntegrity();
    Require(integrity.cell_id == config_.cell_id &&
                integrity.current_generation.nonzero(),
            "active publication is absent or belongs to another cell");
    auto result = publication_store_.ReadActiveBundlePublication(
        integrity.current_generation);
    Require(result.ok() && result.value().valid() &&
                result.value().publication_generation ==
                    integrity.current_generation &&
                result.value().durable_public_pointer_content_digest.value() ==
                    integrity.current_pointer_digest,
            "active publication cannot be independently verified");
    auto bundle = bundle_source_.get_verified(
        result.value().request.candidate_retained_bundle_id);
    Require(bundle != nullptr,
            "active publication's exact retained bundle is unavailable");
    Require(bundle->value().cell_graph.cell_id == config_.cell_id,
            "active publication's retained bundle names another cell");
    Require(bundle->value().cell_graph.graph_revision ==
                result.value().request.candidate_graph_revision,
            "active publication's retained bundle has another graph revision");
    return {std::move(result.value()), std::move(bundle)};
  }

  [[nodiscard]] core::ExecutionHostEpochDTO ExecutionHostEpoch(
      const core::ActiveBundlePublicationReceiptDTO &publication) const {
    core::ExecutionHostEpochDTO epoch{
        config_.runtime_unit_id, config_.runtime_epoch,
        config_.request_owner_epoch, LedgerDigest(),
        publication.publication_generation};
    Require(epoch.valid(), "constructed execution-host epoch is invalid");
    return epoch;
  }

  [[nodiscard]] core::ConfigurationFenceStatusDTO
  QueryStatusLocked(const core::ConfigurationFenceStatusQueryDTO &query,
                    const InstalledNativeCandidateObservation *candidate = nullptr) const {
    Require(query.valid(), "fence status query is invalid");
    std::unique_lock admission_lock(*admission_gate_.shared_admission_mutex(),
                                    std::defer_lock);
    if (HasProviderProof(query.expected_scope))
      admission_lock.lock();
    const auto row = ReadByFence(query.configuration_fence_id,
                                 query.configuration_fence_generation);
    Require(row.has_value(), "fence status identity is unknown");
    Require(!candidate || row->state == 0,
            "installed NativeApply observation no longer has a held fence");
    Require(row->owner_kind == DurableFenceOwnerKind::kApply,
            "Apply status query names a route-configuration fence");
    const auto intent = DecodeRequest<core::ApplyConfigurationFenceIntentDTO>(
        row->intent_bytes);
    const auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
        row->accepted_bytes);
    Require(intent.owner_transaction == query.owner_transaction &&
                accepted.configuration_fence_id ==
                    query.configuration_fence_id &&
                accepted.configuration_fence_generation ==
                    query.configuration_fence_generation &&
                accepted.captured_scope == query.expected_scope,
            "fence status query conflicts with the durable fence");
    if (row->state == 1) {
      Require(row->release_receipt_bytes.has_value(),
              "released fence lacks its durable receipt");
      const auto released = DecodeResponse<core::ConfigurationFenceReleasedDTO>(
          *row->release_receipt_bytes);
      core::ConfigurationFenceStatusDTO status{
          query,
          released.request.reconciliation_evidence,
          core::ConfigurationFenceState::kReleased,
          false,
          released,
          {},
          Digest("manyforge.kernel-fence-status.v1", EncodeRequest(query),
                 released.release_receipt_digest.value()),
          std::nullopt};
      Require(status.presence_valid(),
              "constructed released fence status is invalid");
      return status;
    }

    const auto active = ReadActivePublication();
    if (candidate) {
      Require(row->state == 0 && active.receipt == candidate->predecessor &&
                  active.receipt.request.candidate_retained_bundle_id == intent.expected_retained_bundle_id &&
                  active.receipt.request.candidate_graph_revision == intent.expected_graph_revision &&
                  admission_gate_.exact_fence_is_held(query.configuration_fence_id, query.expected_scope),
              "installed NativeApply lost its exact held predecessor");
      const auto kind = candidate->parent.intent.claim_kind;
      Require((kind == core::CellAdmissionClaimKindDTO::kManagerApply &&
                   candidate->parent.intent.owner_transaction_id == query.owner_transaction.apply_id.value()) ||
                  kind == core::CellAdmissionClaimKindDTO::kAuthoringMutation,
              "installed NativeApply parent has another transaction");
      Require(static_cast<bool>(config_.selected_claim_recovery_receipt),
              "installed NativeApply has no actual held parent source");
      const core::CellAdmissionRecoveryReceiptQueryDTO parent_query{
          candidate->parent.intent, candidate->parent, std::nullopt};
      const auto held = config_.selected_claim_recovery_receipt(parent_query);
      Require(core::ValidateCellAdmissionRecoveryReceiptQuery(parent_query).ok() &&
                  held.ok() && core::ValidateCellAdmissionRecoveryReceipt(held.value()).ok() &&
                  held.value().status == core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
                  held.value().matched_claim == std::optional(candidate->parent),
              "installed NativeApply parent is no longer held exact");
      ValidateInstalledCandidateConfigurations(query.expected_scope, *candidate->bundle, candidate->lineages);
      // Join the actual installed records to their current Kernel rows, not
      // just to an epoch/generation projection. Eligibility is not inferred.
      for (const auto &fact : candidate->facts) {
        for (std::size_t index = 0; index < fact.domains.size(); ++index) {
          const auto &domain = fact.domains[index];
          if (!domain.provider()) continue;
          const auto scoped = std::find_if(query.expected_scope.domains.begin(), query.expected_scope.domains.end(),
              [&](const auto &row) { return row.control_domain_id == domain.control_domain_id; });
          if (scoped == query.expected_scope.domains.end()) continue;
          const auto current = config_.provider_configuration_source(domain.control_domain_id);
          Require(current.ok() && current.value() &&
                      current.value()->retained_bundle_id == fact.retained_bundle_id &&
                      current.value()->graph_revision == fact.graph_revision &&
                      current.value()->domain == domain &&
                      current.value()->provider_occurrence == fact.provider_occurrences[index] &&
                      current.value()->component_lifecycle_generation == fact.component_lifecycle_generation,
                  "installed NativeApply facts no longer match their current Kernel rows");
        }
      }
    } else {
      ValidateProviderConfigurations(query.expected_scope, true, *active.bundle);
    }
    const auto observation = admission_gate_.Observe(query.expected_scope);
    const auto configurations = SelectConfigurations(
        observation.active_control_configurations, query.expected_scope, true);
    const auto epoch = ExecutionHostEpoch(active.receipt);
    core::ConfigurationFenceReconciliationEvidenceDTO evidence{
        epoch,
        query.owner_transaction,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        observation.operation_resource_generation,
        active.receipt.request.candidate_graph_revision,
        active.receipt.request.candidate_projection_revision,
        active.receipt.publication_generation,
        active.receipt.durable_public_pointer_content_digest,
        configurations,
        observation.affected_queued_operation_count,
        observation.affected_active_operation_count,
        Digest("manyforge.kernel-fence-reconciliation-evidence.seed.v1")};
    Require(evidence.valid(),
            "constructed fence reconciliation evidence is invalid");
    core::ConfigurationFenceStatusDTO status{
        query,
        evidence,
        core::ConfigurationFenceState::kHeld,
        true,
        std::nullopt,
        {},
        Digest("manyforge.kernel-fence-status.seed.v1"),
        std::nullopt};
    evidence.evidence_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(status)));
    status.reconciliation_evidence = evidence;
    status.result_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(status)));
    Require(status.presence_valid(),
            "constructed held fence status is invalid");
    return status;
  }

  ManagedKernelApplyFenceOwnerConfig config_;
  ManagedKernelAdmissionGate &admission_gate_;
  FileLock lock_;
  sqlite3 *database_{};
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source_;
  component_manager::SqliteActiveBundlePublicationStore publication_store_;
  mutable std::mutex mutex_;
};

ManagedKernelApplyFenceOwner::ManagedKernelApplyFenceOwner(
    ManagedKernelApplyFenceOwnerConfig config,
    ManagedKernelAdmissionGate &admission_gate)
    : impl_(std::make_unique<Impl>(std::move(config), admission_gate)) {}

ManagedKernelApplyFenceOwner::~ManagedKernelApplyFenceOwner() = default;

core::ConfigurationFenceAdmissionSnapshotDTO
ManagedKernelApplyFenceOwner::ObserveAdmission(
    const core::ConfigurationFenceAdmissionQueryDTO &query) const {
  return impl_->ObserveAdmission(query);
}

core::ConfigurationFenceAcceptedDTO ManagedKernelApplyFenceOwner::Acquire(
    const core::ApplyConfigurationFenceIntentDTO &intent) {
  return impl_->Acquire(intent);
}

core::ConfigurationFenceAcceptedDTO ManagedKernelApplyFenceOwner::Acquire(
    const core::ApplyConfigurationFenceIntentDTO &intent,
    const ManagedTerminalApplyFenceValidator &validate_terminal_overlap) {
  return impl_->Acquire(intent, validate_terminal_overlap);
}

core::ApplyFenceAcquireAttemptReconcileResultDTO
ManagedKernelApplyFenceOwner::ReconcileAcquireAttempt(
    const core::ApplyFenceAcquireAttemptQueryDTO &query) const {
  return impl_->ReconcileAcquireAttempt(query);
}

core::ApplyFenceReconcileResultDTO ManagedKernelApplyFenceOwner::Rehydrate(
    const core::ApplyFenceRehydrateDTO &request) {
  return impl_->Rehydrate(request);
}

core::ConfigurationFenceStatusDTO ManagedKernelApplyFenceOwner::QueryStatus(
    const core::ConfigurationFenceStatusQueryDTO &query) const {
  return impl_->QueryStatus(query);
}

core::ConfigurationFenceStatusDTO
ManagedKernelApplyFenceOwner::QueryInstalledNativeApplyCandidateStatus(
    const core::ConfigurationFenceStatusQueryDTO &query,
    const core::ActiveBundlePublicationReceiptDTO &predecessor,
    const core::CellAdmissionClaimDTO &parent,
    const core::NativeApplyPlanDTO &plan,
    const std::vector<core::InstalledProviderCandidateFactsDTO> &facts,
    const std::function<void()> &revalidate_installed_attempt) const {
  return impl_->QueryInstalledNativeApplyCandidateStatus(
      query, predecessor, parent, plan, facts, revalidate_installed_attempt);
}

void ManagedKernelApplyFenceOwner::RunWithExactHeldPredecessor(
    const core::ConfigurationFenceStatusQueryDTO &query,
    const core::ActiveBundlePublicationGeneration
        &expected_predecessor_publication_generation,
    const core::ContentDigest &expected_predecessor_public_pointer_digest,
    const std::function<void()> &effect) const {
  impl_->RunWithExactHeldPredecessor(
      query, expected_predecessor_publication_generation,
      expected_predecessor_public_pointer_digest, effect);
}

void ManagedKernelApplyFenceOwner::RunWithExactHeldOriginalApplyCandidate(
    const core::ConfigurationFenceStatusQueryDTO &query,
    core::ActiveBundlePublicationGeneration predecessor_generation,
    const core::ContentDigest &predecessor_pointer_digest,
    const core::ApplyComponentBringUpAuthorityDTO &parent,
    const core::ProjectionRevisionDTO &candidate,
    const std::vector<core::RuntimeProviderDomainConfigurationReadbackDTO>
        &actual_candidate_configurations,
    const std::function<void()> &effect) const {
  impl_->RunWithExactHeldOriginalApplyCandidate(
      query, predecessor_generation, predecessor_pointer_digest, parent,
      candidate, actual_candidate_configurations, effect);
}

void ManagedKernelApplyFenceOwner::RunWithExactHeldSuccessor(
    const core::ConfigurationFenceStatusQueryDTO &query,
    const core::ActiveBundlePublicationReceiptDTO &successor,
    const std::function<void()> &effect) const {
  impl_->RunWithExactHeldSuccessor(query, successor, effect);
}

void ManagedKernelApplyFenceOwner::RunWithExactHeldPredecessorReadback(
    const core::ConfigurationFenceStatusQueryDTO &query,
    const core::ActiveBundlePublicationGeneration &generation,
    const core::ContentDigest &pointer_digest,
    const std::function<void(const HeldStatusView&)> &effect) const {
  impl_->RunWithExactHeldPredecessorReadback(query, generation, pointer_digest,
      [&](const auto& status) { WithHeldStatusView(status, effect); });
}

void ManagedKernelApplyFenceOwner::RunWithExactHeldSuccessorReadback(
    const core::ConfigurationFenceStatusQueryDTO &query,
    const core::ActiveBundlePublicationReceiptDTO &successor,
    const std::function<void(const HeldStatusView&)> &effect) const {
  impl_->RunWithExactHeldSuccessorReadback(query, successor,
      [&](const auto& status) { WithHeldStatusView(status, effect); });
}

core::ConfigurationFenceReleasedDTO ManagedKernelApplyFenceOwner::Release(
    const core::ConfigurationFenceReleaseDTO &request) {
  return impl_->Release(request);
}

core::Result<core::RouteConfigurationFenceAdmissionSnapshotDTO>
ManagedKernelApplyFenceOwner::ObserveRouteConfigurationFenceAdmission(
    const core::RouteConfigurationFenceAdmissionQueryDTO &query) const {
  return InvokeRouteFencePort<
      core::RouteConfigurationFenceAdmissionSnapshotDTO>(
      [&] { return impl_->ObserveRouteConfigurationFenceAdmission(query); });
}

core::Result<core::RouteConfigurationFenceAcceptedDTO>
ManagedKernelApplyFenceOwner::AcquireRouteConfigurationFence(
    const core::RouteConfigurationFenceIntentDTO &intent) {
  return InvokeRouteFencePort<core::RouteConfigurationFenceAcceptedDTO>(
      [&] { return impl_->AcquireRouteConfigurationFence(intent); });
}

core::Result<core::RouteConfigurationFenceAcquireAttemptReconciliationDTO>
ManagedKernelApplyFenceOwner::ReconcileRouteConfigurationFenceAcquireAttempt(
    const core::RouteConfigurationFenceIntentDTO &intent) const {
  return InvokeRouteFencePort<
      core::RouteConfigurationFenceAcquireAttemptReconciliationDTO>([&] {
    return impl_->ReconcileRouteConfigurationFenceAcquireAttempt(intent);
  });
}

core::Result<core::RouteConfigurationFenceStatusDTO>
ManagedKernelApplyFenceOwner::ObserveRouteConfigurationFenceStatus(
    const core::RouteConfigurationFenceStatusQueryDTO &query) const {
  return InvokeRouteFencePort<core::RouteConfigurationFenceStatusDTO>(
      [&] { return impl_->ObserveRouteConfigurationFenceStatus(query); });
}

core::Result<core::RouteConfigurationFenceReleasedDTO>
ManagedKernelApplyFenceOwner::ReleaseRouteConfigurationFence(
    const core::RouteConfigurationFenceReleaseRequestDTO &request) {
  return InvokeRouteFencePort<core::RouteConfigurationFenceReleasedDTO>(
      [&] { return impl_->ReleaseRouteConfigurationFence(request); });
}

core::Result<core::RouteConfigurationFenceReleaseAttemptReconciliationDTO>
ManagedKernelApplyFenceOwner::ReconcileRouteConfigurationFenceReleaseAttempt(
    const core::RouteConfigurationFenceReleaseRequestDTO &request) const {
  return InvokeRouteFencePort<
      core::RouteConfigurationFenceReleaseAttemptReconciliationDTO>([&] {
    return impl_->ReconcileRouteConfigurationFenceReleaseAttempt(request);
  });
}

namespace {
template <typename Value, typename Operation>
core::Result<Value> InvokeSelectedFencePort(Operation &&operation) noexcept {
  try {
    return core::Result<Value>::Ok(operation());
  } catch (const std::exception &error) {
    return core::Result<Value>::Err(core::ErrorDTO{
        "selected_bring_up_configuration_fence_rejected", error.what(), {}});
  } catch (...) {
    return core::Result<Value>::Err(
        core::ErrorDTO{"selected_bring_up_configuration_fence_rejected",
                       "selected fence operation failed",
                       {}});
  }
}
} // namespace
core::Result<core::ConfigurationFenceAcceptedDTO>
ManagedKernelApplyFenceOwner::AcquireSelectedBringUpFence(
    const core::SelectedBringUpConfigurationFenceIntentDTO &value) {
  return InvokeSelectedFencePort<core::ConfigurationFenceAcceptedDTO>(
      [&] { return impl_->AcquireSelectedBringUpFence(value); });
}
core::Result<core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO>
ManagedKernelApplyFenceOwner::ReconcileSelectedBringUpFenceAcquireAttempt(
    const core::SelectedBringUpFenceAcquireAttemptQueryDTO &value) const {
  return InvokeSelectedFencePort<
      core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO>([&] {
    return impl_->ReconcileSelectedBringUpFenceAcquireAttempt(value);
  });
}
core::Result<core::SelectedBringUpConfigurationFenceStatusDTO>
ManagedKernelApplyFenceOwner::ObserveSelectedBringUpFenceStatus(
    const core::SelectedBringUpConfigurationFenceStatusQueryDTO &value) const {
  return InvokeSelectedFencePort<
      core::SelectedBringUpConfigurationFenceStatusDTO>(
      [&] { return impl_->ObserveSelectedBringUpFenceStatus(value); });
}
core::Result<core::SelectedBringUpConfigurationFenceReleasedDTO>
ManagedKernelApplyFenceOwner::ReleaseSelectedBringUpFence(
    const core::SelectedBringUpConfigurationFenceReleaseDTO &value) {
  return InvokeSelectedFencePort<
      core::SelectedBringUpConfigurationFenceReleasedDTO>(
      [&] { return impl_->ReleaseSelectedBringUpFence(value); });
}
core::Result<core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO>
ManagedKernelApplyFenceOwner::ReconcileSelectedBringUpFenceReleaseAttempt(
    const core::SelectedBringUpFenceReleaseAttemptQueryDTO &value) const {
  return InvokeSelectedFencePort<
      core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO>([&] {
    return impl_->ReconcileSelectedBringUpFenceReleaseAttempt(value);
  });
}
void ManagedKernelApplyFenceOwner::RunWithExactHeldSelectedPredecessor(
    const core::SelectedBringUpConfigurationFenceStatusQueryDTO &query,
    const std::function<void()> &effect) const {
  impl_->RunWithExactHeldSelectedPredecessor(query, effect);
}

} // namespace manyforge::roboplan_backend
