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
#include <iostream>
#include <limits>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_content_identity/content_identity.hpp>
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
namespace persistence = manyforge::persistence;

constexpr std::string_view kSchemaId =
    "manyforge.internal.kernel-configuration-fence-ledger.v7";
constexpr int kSchemaVersion = 7;
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
    if (::flock(descriptor_, LOCK_EX | LOCK_NB) == 0) {
      return;
    }
    // A predecessor execution host (e.g. of a Manager that was killed) still
    // owns the ledger until its teardown completes; the kernel releases the
    // lock when it exits. Wait for exactly that release. The Manager's
    // readiness timeout bounds the wait; a stop signal interrupts it.
    if (errno == EWOULDBLOCK) {
      std::cerr << "Kernel Apply-fence ledger is held by a predecessor "
                   "execution host; waiting for it to exit\n";
      if (::flock(descriptor_, LOCK_EX) == 0) {
        return;
      }
    }
    const auto detail = std::string(std::strerror(errno));
    ::close(descriptor_);
    descriptor_ = -1;
    Fail("cannot own the Kernel Apply-fence ledger: " + detail);
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

// The predecessor-side rows an Apply fence pins at acquisition: the
// reconciled configuration of every predecessor controller domain that has
// one. A domain with neither a reconciled configuration nor a route
// publication was never Enabled (or was Disabled): nothing is installed for
// it to pin, so it yields no row, the rule ObservedConfigurations applies for
// releases. A domain that still has a route publication is commandable or
// externally owned (e.g. an unreconciled external route); pinning nothing for
// it would hide live control, so it still refuses.
[[nodiscard]] std::vector<core::DomainActiveControlConfigurationDTO>
SelectConfigurations(
    const std::vector<core::DomainActiveControlConfigurationDTO> &available,
    const std::vector<core::DomainActiveControlConfigurationDTO>
        &route_published,
    const core::ConfigurationFenceScopeDTO &scope) {
  const auto find = [](const auto &rows, const core::ControlDomainId &id) {
    const auto found = std::lower_bound(
        rows.begin(), rows.end(), id,
        [](const auto &configuration, const auto &domain_id) {
          return configuration.control_domain_id < domain_id;
        });
    return found != rows.end() && found->control_domain_id == id ? &*found
                                                                 : nullptr;
  };
  std::vector<core::DomainActiveControlConfigurationDTO> result;
  for (const auto &domain : scope.domains) {
    if (!domain.has_controller_predecessor()) {
      continue;
    }
    if (const auto *found = find(available, domain.control_domain_id)) {
      result.push_back(*found);
      continue;
    }
    Require(find(route_published, domain.control_domain_id) == nullptr,
            "the execution owner has no reconciled active configuration for "
            "an affected domain that still has a route publication");
  }
  return result;
}

// The rows a held fence's status reports: the configurations actually active
// now for the scope's controller domains. A domain with none (new to the
// cell, or not installed yet) has nothing to report or restore, so neither a
// before-effect release nor a post-publication release requires one.
[[nodiscard]] std::vector<core::DomainActiveControlConfigurationDTO>
ObservedConfigurations(
    const std::vector<core::DomainActiveControlConfigurationDTO> &available,
    const core::ConfigurationFenceScopeDTO &scope) {
  std::vector<core::DomainActiveControlConfigurationDTO> result;
  for (const auto &domain : scope.domains) {
    if (!domain.has_controller_side()) {
      continue;
    }
    const auto found = std::lower_bound(
        available.begin(), available.end(), domain.control_domain_id,
        [](const auto &configuration, const auto &domain_id) {
          return configuration.control_domain_id < domain_id;
        });
    if (found != available.end() &&
        found->control_domain_id == domain.control_domain_id) {
      result.push_back(*found);
    }
  }
  return result;
}

[[nodiscard]] std::string
FenceIdFor(const core::ApplyConfigurationFenceIntentDTO &intent,
           const std::uint64_t generation) {
  const auto digest = content_identity::sha256_digest(EncodeRequest(intent));
  return "configuration-fence-" + std::to_string(generation) + "-" +
         digest.substr(digest.size() - 16U);
}

struct DurableFenceRow final {
  std::string intent_bytes;
  std::string accepted_bytes;
  int state;
  std::optional<std::string> release_request_bytes;
  std::optional<std::string> release_receipt_bytes;
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
    return {0U, {}, 0U, 0U, {}};
  }
  const auto snapshot = table->admission_snapshot(scope.resources);
  return {snapshot.operation_resource_generation,
          snapshot.active_control_configurations,
          snapshot.affected_queued_operation_count,
          snapshot.affected_active_operation_count,
          snapshot.route_published_active_control_configurations};
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
  Require(SelectConfigurations(
              observation.active_control_configurations,
              observation.route_published_active_control_configurations,
              scope) == expected_active_control_configurations,
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
public:
  Impl(ManagedKernelApplyFenceOwnerConfig config,
       ManagedKernelAdmissionGate &admission_gate)
      : config_(ValidateConfig(std::move(config))),
        admission_gate_(admission_gate), lock_(config_.ledger_path),
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
    const auto active = ReadActivePublication();
    const auto observation = admission_gate_.Observe(query.affected_scope);
    const auto configurations = SelectConfigurations(
        observation.active_control_configurations,
        observation.route_published_active_control_configurations,
        query.affected_scope);
    const auto epoch = ExecutionHostEpoch(active);
    core::ConfigurationFenceAdmissionSnapshotDTO snapshot{
        query,
        epoch,
        active.request.candidate_projection_revision,
        active.durable_public_pointer_content_digest,
        observation.operation_resource_generation,
        core::ConfigurationFenceGeneration(ReadFenceGeneration()),
        configurations,
        observation.affected_queued_operation_count,
        observation.affected_active_operation_count,
        Digest("manyforge.kernel-fence-admission-observation.seed.v1")};
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
    const auto intent_bytes = EncodeRequest(intent);
    if (const auto existing =
            ReadByRequest(intent.owner_request_context.request_owner_epoch,
                          intent.owner_request_context.request_id)) {
      Require(existing->intent_bytes == intent_bytes && existing->state == 0,
              "request ID is bound to another or already released fence "
              "outcome");
      auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
          existing->accepted_bytes);
      accepted.replay_status = core::RequestReplayStatusDTO::kReplayedExact;
      Require(accepted.matches(intent),
              "durable exact-replay acceptance contradicts its intent");
      return accepted;
    }
    // The ledger is bounded by eviction, never refusal: a full ledger drops
    // its oldest released fence when this one is inserted (see below). Only
    // a ledger full of held fences refuses.
    const bool ledger_full =
        ReadSingleInteger(database_, "SELECT COUNT(*) FROM kernel_apply_fences",
                          "durable fence record count") >= kMaximumFenceRecords;
    Require(!ledger_full ||
                ReadSingleInteger(database_,
                                  "SELECT COUNT(*) FROM kernel_apply_fences "
                                  "WHERE state=1",
                                  "durable released fence count") > 0,
            "durable fence ledger is full of held fences");

    const auto active = ReadActivePublication();
    Require(active.request.candidate_retained_bundle_id ==
                    intent.expected_retained_bundle_id &&
                active.request.candidate_graph_revision ==
                    intent.expected_graph_revision,
            "active predecessor changed before fence acquisition");
    const auto observed = admission_gate_.Observe(intent.scope);
    Require(observed.operation_resource_generation ==
                    intent.expected_operation_resource_generation &&
                observed.affected_queued_operation_count == 0U &&
                observed.affected_active_operation_count == 0U,
            "affected operation admission is busy or changed");
    const auto active_configurations = SelectConfigurations(
        observed.active_control_configurations,
        observed.route_published_active_control_configurations, intent.scope);
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
          if (ledger_full) {
            // The oldest released fence is settled history. Held fences are
            // never evicted (RestoreHeldFence, HeldApplyFence and the overlap
            // check read only held rows). A released row serves only exact
            // Release replay and status queries for its Apply, which the
            // Manager settles from its own durable release record long before
            // kMaximumFenceRecords newer fences exist. The ledger digest
            // changes with this insert anyway.
            Statement evict(
                database_,
                "DELETE FROM kernel_apply_fences WHERE fence_generation=("
                "SELECT MIN(fence_generation) FROM kernel_apply_fences "
                "WHERE state=1)");
            Require(evict.Step() == SQLITE_DONE &&
                        sqlite3_changes(database_) == 1,
                    "durable fence ledger eviction failed");
          }
          Statement insert(
              database_,
              "INSERT INTO kernel_apply_fences ("
              "owner_id,owner_generation,request_id,request_owner_epoch,"
              "intent_blob,accepted_blob,fence_id,fence_generation,state,"
              "release_request_blob,release_receipt_blob) "
              "VALUES (?,?,?,?,?,?,?,?,0,NULL,NULL)");
          insert.Text(1, intent.owner_transaction.apply_id.value());
          insert.Integer(2, intent.owner_transaction.apply_generation.value());
          insert.Text(3, intent.owner_request_context.request_id.value());
          insert.Text(4,
                      intent.owner_request_context.request_owner_epoch.value());
          insert.Blob(5, intent_bytes);
          insert.Blob(6, EncodeResponse(accepted));
          insert.Text(7, accepted.configuration_fence_id.value());
          insert.Integer(8, accepted.configuration_fence_generation.value());
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
                    "SELECT intent_blob,accepted_blob,state,"
                    "release_request_blob,release_receipt_blob "
                    "FROM kernel_apply_fences WHERE fence_id=?");
                statement.Text(1, id.value());
                const auto old = ReadOptionalRow(statement);
                Require(
                    old && old->state == 0,
                    "overlapping barrier is not a held Apply fence");
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
    const auto epoch = ExecutionHostEpoch(active);
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
      if (existing->intent_bytes == intent_bytes && existing->state == 0) {
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
    const auto epoch = ExecutionHostEpoch(active);
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

  [[nodiscard]] std::optional<core::ConfigurationFenceStatusQueryDTO>
  HeldApplyFence(const core::NamedApplyPublicationTransactionDTO &apply,
                 const core::RuntimeEpoch &manager_runtime_epoch) const {
    std::lock_guard lock(mutex_);
    Require(apply.valid() && !manager_runtime_epoch.empty(),
            "held Apply-fence lookup is invalid");
    Statement statement(database_,
                        "SELECT accepted_blob FROM kernel_apply_fences WHERE "
                        "owner_id=? AND owner_generation=? AND state=0");
    statement.Text(1, apply.apply_id.value());
    statement.Integer(2, apply.apply_generation.value());
    if (statement.Step() != SQLITE_ROW)
      return std::nullopt;
    const auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
        statement.ColumnBlob(0));
    Require(statement.Step() == SQLITE_DONE,
            "held Apply fence is not unique");
    return core::ConfigurationFenceStatusQueryDTO{
        {apply.apply_id, apply.apply_generation},
        accepted.configuration_fence_id,
        accepted.configuration_fence_generation,
        accepted.captured_scope,
        manager_runtime_epoch};
  }

  void RunWithExactHeldPredecessor(
      const core::ConfigurationFenceStatusQueryDTO &query,
      const core::ActiveBundlePublicationGeneration
          &expected_predecessor_publication_generation,
      const core::ContentDigest &expected_predecessor_public_pointer_digest,
      const std::function<void()> &effect) const {
    std::lock_guard lock(mutex_);
    Require(query.valid() &&
                expected_predecessor_publication_generation.nonzero() &&
                !expected_predecessor_public_pointer_digest.empty() &&
                static_cast<bool>(effect),
            "fenced runtime-composition installation request is invalid");
    const auto row = ReadByFence(query.configuration_fence_id,
                                 query.configuration_fence_generation);
    Require(row.has_value() && row->state == 0,
            "runtime composition does not name a durable held fence");
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
    Require(active.request.candidate_retained_bundle_id ==
                    intent.expected_retained_bundle_id &&
                active.request.candidate_graph_revision ==
                    intent.expected_graph_revision &&
                active.publication_generation ==
                    expected_predecessor_publication_generation &&
                active.durable_public_pointer_content_digest ==
                    expected_predecessor_public_pointer_digest,
            "runtime composition predecessor publication changed while its "
            "fence was held");
    admission_gate_.RunWithExactHeldFence(query.configuration_fence_id,
                                          query.expected_scope, effect);
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
    Require(row.has_value() && row->state == 0,
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
    Require(active == successor,
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

  [[nodiscard]] static ManagedKernelApplyFenceOwnerConfig
  ValidateConfig(ManagedKernelApplyFenceOwnerConfig config) {
    config.ledger_path = ValidateLedgerPath(std::move(config.ledger_path));
    config.manager_state_root = ExistingPlainDirectory(
        std::move(config.manager_state_root), "Manager state root");
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
          "owner_id TEXT NOT NULL,"
          "owner_generation INTEGER NOT NULL CHECK("
          "owner_generation>0),request_id TEXT NOT NULL,request_owner_epoch "
          "TEXT NOT NULL,intent_blob BLOB NOT NULL,accepted_blob BLOB NOT NULL,"
          "fence_id TEXT NOT NULL,fence_generation INTEGER NOT NULL CHECK("
          "fence_generation>0),state INTEGER NOT NULL CHECK(state IN (0,1)),"
          "release_request_blob BLOB,release_receipt_blob BLOB,"
          "PRIMARY KEY(request_owner_epoch,request_id),"
          "UNIQUE(owner_id,owner_generation),UNIQUE(fence_id),"
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
                        "SELECT accepted_blob FROM kernel_apply_fences WHERE "
                        "state=0 ORDER BY fence_generation");
    while (statement.Step() == SQLITE_ROW) {
      const auto accepted = DecodeResponse<core::ConfigurationFenceAcceptedDTO>(
          statement.ColumnBlob(0));
      admission_gate_.RestoreFence(accepted.configuration_fence_id,
                                   accepted.captured_scope);
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
                        "SELECT intent_blob,accepted_blob,state,"
                        "release_request_blob,release_receipt_blob FROM "
                        "kernel_apply_fences WHERE request_owner_epoch=? AND "
                        "request_id=?");
    statement.Text(1, owner_epoch.value());
    statement.Text(2, request_id.value());
    return ReadOptionalRow(statement);
  }

  [[nodiscard]] std::optional<DurableFenceRow>
  ReadByFence(const core::ConfigurationFenceId &fence_id,
              const core::ConfigurationFenceGeneration generation) const {
    Statement statement(
        database_,
        "SELECT intent_blob,accepted_blob,state,release_request_blob,"
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
    DurableFenceRow row{statement.ColumnBlob(0), statement.ColumnBlob(1),
                        static_cast<int>(statement.ColumnInteger(2)),
                        statement.OptionalColumnBlob(3),
                        statement.OptionalColumnBlob(4)};
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
        database_, "SELECT intent_blob,accepted_blob,state,"
                   "release_request_blob,release_receipt_blob FROM "
                   "kernel_apply_fences ORDER BY fence_generation");
    while (statement.Step() == SQLITE_ROW) {
      AppendField(bytes, statement.ColumnBlob(0));
      AppendField(bytes, statement.ColumnBlob(1));
      AppendField(bytes, std::to_string(statement.ColumnInteger(2)));
      AppendField(bytes, statement.OptionalColumnBlob(3).value_or(""));
      AppendField(bytes, statement.OptionalColumnBlob(4).value_or(""));
    }
    return core::ContentDigest(content_identity::sha256_digest(bytes));
  }

  [[nodiscard]] core::ActiveBundlePublicationReceiptDTO ReadActivePublication() const {
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
    return std::move(result.value());
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
  QueryStatusLocked(const core::ConfigurationFenceStatusQueryDTO &query) const {
    Require(query.valid(), "fence status query is invalid");
    const auto row = ReadByFence(query.configuration_fence_id,
                                 query.configuration_fence_generation);
    Require(row.has_value(), "fence status identity is unknown");
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
    const auto observation = admission_gate_.Observe(query.expected_scope);
    const auto configurations = ObservedConfigurations(
        observation.active_control_configurations, query.expected_scope);
    const auto epoch = ExecutionHostEpoch(active);
    core::ConfigurationFenceReconciliationEvidenceDTO evidence{
        epoch,
        query.owner_transaction,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        observation.operation_resource_generation,
        active.request.candidate_graph_revision,
        active.request.candidate_projection_revision,
        active.publication_generation,
        active.durable_public_pointer_content_digest,
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

std::optional<core::ConfigurationFenceStatusQueryDTO>
ManagedKernelApplyFenceOwner::HeldApplyFence(
    const core::NamedApplyPublicationTransactionDTO &apply,
    const core::RuntimeEpoch &manager_runtime_epoch) const {
  return impl_->HeldApplyFence(apply, manager_runtime_epoch);
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

} // namespace manyforge::roboplan_backend
