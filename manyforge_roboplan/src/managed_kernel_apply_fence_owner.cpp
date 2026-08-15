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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/disable_handoff_record_codec_internal.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/route_fence_record_codec_internal.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_persistence/sqlite_read_only_preflight.hpp>
#include <optional>
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
    "manyforge.internal.kernel-configuration-fence-ledger.v4";
constexpr int kSchemaVersion = 4;
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

template <typename Value, typename Operation>
[[nodiscard]] core::Result<Value>
InvokeDisableHandoffPort(Operation &&operation) noexcept {
  try {
    return core::Result<Value>::Ok(operation());
  } catch (const std::exception &error) {
    return core::Result<Value>::Err(
        core::ErrorDTO{"kernel_disable_handoff_rejected", error.what(), {}});
  } catch (...) {
    return core::Result<Value>::Err(
        core::ErrorDTO{"kernel_disable_handoff_rejected",
                       "Kernel Disable-handoff operation failed",
                       {}});
  }
}

[[nodiscard]] std::filesystem::path
ExistingPlainDirectory(std::filesystem::path path,
                       const std::string_view label) {
  Require(!path.empty(), std::string(label) + " is empty");
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
  Require(application_id == static_cast<std::uint64_t>(kSqliteApplicationId) &&
              user_version == static_cast<std::uint64_t>(kSchemaVersion) &&
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
    if (!required) {
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

[[nodiscard]] std::vector<core::CurrentDomainActiveControlConfigurationDTO>
SelectCurrentConfigurations(
    const std::vector<core::DomainActiveControlConfigurationDTO> &available,
    const core::ConfigurationFenceScopeDTO &scope) {
  ValidateConfigurations(available);
  std::vector<core::CurrentDomainActiveControlConfigurationDTO> result;
  result.reserve(scope.domains.size());
  for (const auto &domain : scope.domains) {
    const auto found = std::lower_bound(
        available.begin(), available.end(), domain.control_domain_id,
        [](const auto &configuration, const auto &domain_id) {
          return configuration.control_domain_id < domain_id;
        });
    if (found == available.end() ||
        found->control_domain_id != domain.control_domain_id) {
      result.push_back(
          {domain.control_domain_id,
           {core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
            std::nullopt, std::nullopt}});
    } else {
      result.push_back(
          {domain.control_domain_id,
           {core::ExpectedActiveControlConfigurationPresenceDTO::kPresent,
            found->active_control_configuration_id,
            found->active_control_configuration_generation}});
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

[[nodiscard]] std::string
FenceIdFor(const core::RouteConfigurationFenceIntentDTO &intent,
           const std::uint64_t generation) {
  const auto digest = content_identity::sha256_digest(
      core::detail::EncodeRouteFenceIntentRecord(intent));
  return "configuration-fence-" + std::to_string(generation) + "-" +
         digest.substr(digest.size() - 16U);
}

[[nodiscard]] std::string
FenceIdFor(const core::DisableHandoffRequestDTO &request,
           const std::uint64_t generation) {
  const auto digest = content_identity::sha256_digest(
      core::detail::EncodeDisableHandoffRequestRecord(request));
  return "configuration-fence-" + std::to_string(generation) + "-" +
         digest.substr(digest.size() - 16U);
}

enum class DurableFenceOwnerKind { kApply, kRoute, kDisable };

[[nodiscard]] constexpr std::string_view
ToString(const DurableFenceOwnerKind kind) noexcept {
  switch (kind) {
  case DurableFenceOwnerKind::kApply:
    return "apply";
  case DurableFenceOwnerKind::kRoute:
    return "route";
  case DurableFenceOwnerKind::kDisable:
    return "disable";
  }
  return {};
}

struct DurableFenceRow final {
  DurableFenceOwnerKind owner_kind;
  std::string intent_bytes;
  std::string accepted_bytes;
  std::optional<std::string> disable_fence_scope_bytes;
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

[[nodiscard]] bool
ScopeIsSubset(const core::ConfigurationFenceScopeDTO &subset,
              const core::ConfigurationFenceScopeDTO &superset) noexcept {
  const auto ids_subset = [](const auto &left, const auto &right,
                             const auto &key) {
    return std::all_of(left.begin(), left.end(), [&](const auto &item) {
      const auto found = std::lower_bound(
          right.begin(), right.end(), key(item),
          [&](const auto &candidate, const auto &requested_key) {
            return key(candidate) < requested_key;
          });
      return found != right.end() && *found == item;
    });
  };
  return std::includes(superset.controllable_component_ids.begin(),
                       superset.controllable_component_ids.end(),
                       subset.controllable_component_ids.begin(),
                       subset.controllable_component_ids.end()) &&
         ids_subset(subset.domains, superset.domains,
                    [](const auto &row) { return row.control_domain_id; }) &&
         ids_subset(subset.bindings, superset.bindings,
                    [](const auto &row) { return row.control_binding_id; }) &&
         std::includes(superset.resources.begin(), superset.resources.end(),
                       subset.resources.begin(), subset.resources.end());
}

[[nodiscard]] bool ScopeMembershipIsSubset(
    const core::ConfigurationFenceScopeDTO &subset,
    const core::ConfigurationFenceScopeDTO &superset) noexcept {
  const auto row_ids_are_subset = [](const auto &left, const auto &right,
                                     const auto &key) {
    return std::all_of(left.begin(), left.end(), [&](const auto &item) {
      const auto found = std::lower_bound(
          right.begin(), right.end(), key(item),
          [&](const auto &candidate, const auto &requested_key) {
            return key(candidate) < requested_key;
          });
      return found != right.end() && key(*found) == key(item);
    });
  };
  return std::includes(superset.controllable_component_ids.begin(),
                       superset.controllable_component_ids.end(),
                       subset.controllable_component_ids.begin(),
                       subset.controllable_component_ids.end()) &&
         row_ids_are_subset(
             subset.domains, superset.domains,
             [](const auto &row) { return row.control_domain_id; }) &&
         row_ids_are_subset(
             subset.bindings, superset.bindings,
             [](const auto &row) { return row.control_binding_id; }) &&
         std::includes(superset.resources.begin(), superset.resources.end(),
                       subset.resources.begin(), subset.resources.end());
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
    const std::function<void()> &commit_durable_fence) {
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
  Require(
      std::none_of(
          held_fences_.begin(), held_fences_.end(),
          [&](const auto &held) { return ScopesOverlap(held.scope, scope); }),
      "an overlapping configuration fence already closes operation admission");
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

ManagedKernelAdmissionObservation
ManagedKernelAdmissionGate::AcquireDisableFenceDurably(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::function<void(const ManagedKernelAdmissionObservation &)>
        &commit_durable_fence) {
  Require(!fence_id.empty() && scope.valid() &&
              static_cast<bool>(commit_durable_fence),
          "Disable-fence acquisition is invalid");
  std::lock_guard lock(*admission_mutex_);
  const auto table = resource_table_.lock();
  Require(table != nullptr || !resource_table_was_bound_,
          "the bound Kernel resource table is no longer available");
  Require(std::none_of(held_fences_.begin(), held_fences_.end(),
                       [&](const auto &held) {
                         return ScopesOverlap(held.scope, scope);
                       }),
          "an overlapping configuration fence already closes Disable scope");
  const auto snapshot = table != nullptr
                            ? table->admission_snapshot(scope.resources)
                            : core::ResourceSlotAdmissionSnapshotDTO{};
  ManagedKernelAdmissionObservation observation{
      snapshot.operation_resource_generation,
      snapshot.active_control_configurations,
      snapshot.route_published_active_control_configurations,
      snapshot.affected_queued_operation_count,
      snapshot.affected_active_operation_count};
  held_fences_.push_back({fence_id, scope});
  // Stop containment is deliberately allowed to begin with affected work.
  // The same mutex closes new reservations before the prepared durable row.
  commit_durable_fence(observation);
  return observation;
}

void ManagedKernelAdmissionGate::WidenDisableFenceDurably(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &prior_scope,
    const core::ConfigurationFenceScopeDTO &widened_scope,
    const std::function<void(const ManagedKernelAdmissionObservation &)>
        &commit_durable_widening) {
  Require(!fence_id.empty() && prior_scope.valid() && widened_scope.valid() &&
              ScopeIsSubset(prior_scope, widened_scope) &&
              static_cast<bool>(commit_durable_widening),
          "Disable-fence widening is invalid");
  std::lock_guard lock(*admission_mutex_);
  const auto found = std::find_if(
      held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
        return held.fence_id == fence_id && held.scope == prior_scope;
      });
  Require(found != held_fences_.end(),
          "Disable-fence widening does not name the exact held fence");
  Require(std::none_of(held_fences_.begin(), held_fences_.end(),
                       [&](const auto &held) {
                         return &held != &*found &&
                                ScopesOverlap(held.scope, widened_scope);
                       }),
          "Disable-fence widening overlaps another held fence");
  const auto table = resource_table_.lock();
  Require(table != nullptr || !resource_table_was_bound_,
          "the bound Kernel resource table is no longer available");
  const auto snapshot = table != nullptr
                            ? table->admission_snapshot(widened_scope.resources)
                            : core::ResourceSlotAdmissionSnapshotDTO{};
  ManagedKernelAdmissionObservation observation{
      snapshot.operation_resource_generation,
      snapshot.active_control_configurations,
      snapshot.route_published_active_control_configurations,
      snapshot.affected_queued_operation_count,
      snapshot.affected_active_operation_count};
  found->scope = widened_scope;
  commit_durable_widening(observation);
}

void ManagedKernelAdmissionGate::ReleaseDisableFenceDurably(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::function<void(const ManagedKernelAdmissionObservation &)>
        &commit_durable_release) {
  Require(!fence_id.empty() && scope.valid() &&
              static_cast<bool>(commit_durable_release),
          "Disable-fence release is invalid");
  std::lock_guard lock(*admission_mutex_);
  const auto found = std::find_if(
      held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
        return held.fence_id == fence_id && held.scope == scope;
      });
  Require(found != held_fences_.end(),
          "Disable-fence release does not name the exact held fence");
  const auto table = resource_table_.lock();
  Require(table != nullptr || !resource_table_was_bound_,
          "the bound Kernel resource table is no longer available");
  const auto snapshot = table != nullptr
                            ? table->admission_snapshot(scope.resources)
                            : core::ResourceSlotAdmissionSnapshotDTO{};
  ManagedKernelAdmissionObservation observation{
      snapshot.operation_resource_generation,
      snapshot.active_control_configurations,
      snapshot.route_published_active_control_configurations,
      snapshot.affected_queued_operation_count,
      snapshot.affected_active_operation_count};
  commit_durable_release(observation);
  held_fences_.erase(found);
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
  held_fences_.erase(found);
}

void ManagedKernelAdmissionGate::RunWithExactHeldFence(
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::function<void()> &effect) {
  Require(!fence_id.empty() && scope.valid() && static_cast<bool>(effect),
          "fenced runtime-composition effect is invalid");
  std::lock_guard lock(*admission_mutex_);
  const auto found = std::find_if(
      held_fences_.begin(), held_fences_.end(), [&](const auto &held) {
        return held.fence_id == fence_id && held.scope == scope;
      });
  Require(found != held_fences_.end(),
          "runtime composition does not name the exact held fence");
  effect();
}

class ManagedKernelApplyFenceOwner::Impl final {
public:
  Impl(ManagedKernelApplyFenceOwnerConfig config,
       ManagedKernelAdmissionGate &admission_gate)
      : config_(ValidateConfig(std::move(config))),
        admission_gate_(admission_gate), lock_(config_.ledger_path),
        bundle_source_(config_.retained_package_root),
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
        Digest("manyforge.kernel-fence-admission-observation.seed.v1")};
    snapshot.result_digest = core::ContentDigest(
        content_identity::sha256_digest(EncodeResponse(snapshot)));
    Require(snapshot.presence_valid(),
            "constructed admission observation is invalid");
    return snapshot;
  }

  [[nodiscard]] core::ConfigurationFenceAcceptedDTO
  Acquire(const core::ApplyConfigurationFenceIntentDTO &intent) {
    std::lock_guard lock(mutex_);
    Require(intent.valid(), "Apply-fence intent is invalid");
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
        intent.expected_active_control_configurations, [&] {
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
        });
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
      entry.state = exact && row->state == 0
                        ? core::ApplyFenceReconcileState::kMatched
                        : core::ApplyFenceReconcileState::kConflicting;
      if (entry.state == core::ApplyFenceReconcileState::kMatched) {
        admission_gate_.RestoreFence(accepted.configuration_fence_id,
                                     accepted.captured_scope);
      }
    }
    const bool reconciled =
        entry.state == core::ApplyFenceReconcileState::kMatched;
    core::ApplyFenceReconcileResultDTO result{
        epoch,
        {entry},
        !reconciled,
        reconciled ? std::string{}
                   : "keep affected admission closed and reconcile the exact "
                     "durable Kernel fence",
        Digest("manyforge.kernel-fence-rehydration.seed.v1"),
        reconciled ? std::optional<core::ErrorDTO>{}
                   : std::optional<core::ErrorDTO>{core::ErrorDTO{
                         "configuration_fence_rehydration_failed",
                         "the durable Kernel fence is missing or conflicting",
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
                                          query.expected_scope, effect);
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

  void
  BindDisableHandoffKernel(core::ResourceAddressedExecutionKernel &kernel) {
    std::lock_guard lock(mutex_);
    Require(kernel.request_owner_epoch() == config_.request_owner_epoch,
            "Disable-handoff Kernel owner epoch differs from the durable "
            "fence owner");
    Require(disable_handoff_kernel_ == nullptr ||
                disable_handoff_kernel_ == &kernel,
            "Disable-handoff Kernel cannot be rebound to another owner");
    disable_handoff_kernel_ = &kernel;
  }

  [[nodiscard]] core::DisableHandoffAdmissionSnapshotDTO
  ObserveDisableHandoffAdmission(
      const core::DisableHandoffAdmissionQueryDTO &query) const {
    std::lock_guard lock(mutex_);
    return ObserveDisableHandoffAdmissionLocked(query);
  }

  [[nodiscard]] core::DisableHandoffAcceptedDTO
  BeginDisableHandoff(const core::DisableHandoffRequestDTO &request) {
    std::lock_guard lock(mutex_);
    Require(request.locally_valid(), "Disable-handoff request is invalid");
    Require(request.owner_request_context.request_owner_epoch ==
                config_.request_owner_epoch,
            "Disable-handoff request names a stale Kernel owner epoch");
    const auto request_bytes =
        core::detail::EncodeDisableHandoffRequestRecord(request);
    if (const auto existing =
            ReadByRequest(request.owner_request_context.request_owner_epoch,
                          request.owner_request_context.request_id)) {
      Require(existing->owner_kind == DurableFenceOwnerKind::kDisable &&
                  existing->intent_bytes == request_bytes,
              "request ID is bound to another configuration-fence outcome");
      Require(!existing->accepted_bytes.empty(),
              "prepared Disable handoff has an unresolved possible effect; "
              "attempt reconciliation is required");
      auto accepted = core::detail::DecodeDisableHandoffAcceptedRecord(
          existing->accepted_bytes);
      accepted.replay_status = core::RequestReplayStatusDTO::kReplayedExact;
      Require(accepted.matches(request),
              "durable Disable-handoff replay contradicts its request");
      return accepted;
    }
    Require(
        ReadSingleInteger(database_, "SELECT COUNT(*) FROM kernel_apply_fences",
                          "durable fence record count") < kMaximumFenceRecords,
        "durable fence ledger has reached its fixed record bound");

    auto capture = RevalidateDisableHandoffRequestLocked(request);
    if (capture.runtime.scope_resolution !=
            core::DisableHandoffScopeResolutionDTO::kPromotedToFull &&
        (!capture.runtime.affected_queued_operations.empty() ||
         capture.runtime.affected_active_operation_count != 0U)) {
      const core::DisableHandoffAdmissionQueryDTO full_query{
          request.controllable_component_id,
          core::DisableHandoffScopeRequestKindDTO::kFullStopRequired,
          std::nullopt};
      capture = CaptureDisableRuntimeLocked(full_query);
    }
    Require(request.scope_resolution !=
                    core::DisableHandoffScopeResolutionDTO::kPromotedToFull ||
                capture.runtime.scope_resolution ==
                    core::DisableHandoffScopeResolutionDTO::kPromotedToFull,
            "Disable-handoff revalidation narrowed a prior Full-Stop request");
    Require(ScopeMembershipIsSubset(request.affected_scope,
                                    capture.runtime.current_scope),
            "Disable-handoff revalidation narrowed the persisted scope");

    const auto predecessor_generation = ReadFenceGeneration();
    Require(predecessor_generation < std::numeric_limits<std::uint64_t>::max(),
            "configuration-fence generation overflowed");
    const auto next_generation = predecessor_generation + 1U;
    const core::ConfigurationFenceId fence_id(
        FenceIdFor(request, next_generation));
    const core::detail::DisableHandoffFenceScopeRecordDTO scope_record{
        capture.runtime.scope_resolution, capture.runtime.current_scope};
    const auto scope_record_bytes =
        core::detail::EncodeDisableHandoffFenceScopeRecord(scope_record);

    const auto admitted = admission_gate_.AcquireDisableFenceDurably(
        fence_id, scope_record.scope,
        [&](const ManagedKernelAdmissionObservation &observation) {
          const auto selected_configurations = SelectCurrentConfigurations(
              observation.active_control_configurations, scope_record.scope);
          Require(
              capture.active.receipt.request.candidate_retained_bundle_id ==
                      capture.runtime.retained_bundle_id &&
                  capture.active.receipt.request.candidate_graph_revision ==
                      capture.runtime.graph_revision,
              "active publication changed before Disable-fence acquisition");
          Transaction transaction(database_);
          Statement insert(
              database_,
              "INSERT INTO kernel_apply_fences (owner_kind,owner_id,"
              "owner_generation,request_id,request_owner_epoch,intent_blob,"
              "accepted_blob,disable_fence_scope_blob,fence_id,"
              "fence_generation,state,release_request_blob,"
              "release_receipt_blob) VALUES "
              "(?,?,?,?,?,?,NULL,?,?,?,0,NULL,NULL)");
          insert.Text(1, ToString(DurableFenceOwnerKind::kDisable));
          insert.Text(2, request.lifecycle_transaction_id.value());
          insert.Integer(3, request.lifecycle_transaction_generation.value());
          insert.Text(4, request.owner_request_context.request_id.value());
          insert.Text(
              5, request.owner_request_context.request_owner_epoch.value());
          insert.Blob(6, request_bytes);
          insert.Blob(7, scope_record_bytes);
          insert.Text(8, fence_id.value());
          insert.Integer(9, next_generation);
          Require(insert.Step() == SQLITE_DONE,
                  "durable prepared Disable fence insertion returned a row");
          Statement update(
              database_,
              "UPDATE kernel_apply_fence_meta SET fence_generation=? WHERE "
              "id=1 AND fence_generation=?");
          update.Integer(1, next_generation);
          update.Integer(2, predecessor_generation);
          Require(update.Step() == SQLITE_DONE &&
                      sqlite3_changes(database_) == 1,
                  "durable Disable-fence generation compare-and-swap failed");
          transaction.Commit();
          capture.runtime.operation_resource_generation =
              observation.operation_resource_generation;
          capture.runtime.current_active_control_configurations =
              selected_configurations;
        });

    std::optional<core::StopAcceptedDTO> stop_acceptance;
    if (admitted.affected_queued_operation_count != 0U ||
        admitted.affected_active_operation_count != 0U) {
      Require(disable_handoff_kernel_ != nullptr,
              "Disable-handoff Kernel is not bound after durable containment");
      const core::FullStopIntentDTO intent{
          request.owner_request_context,
          capture.runtime.current_cell_stop_generation,
          {}};
      auto result = disable_handoff_kernel_->full_stop(intent);
      if (!result.ok()) {
        Fail("durable Disable containment could not hand off Full Stop: " +
             result.error().message);
      }
      stop_acceptance = std::move(result.value());
    }

    const auto scope_digest =
        core::ComputeDisableHandoffScopeDigest(scope_record.scope);
    Require(scope_digest.ok(),
            "Disable-handoff actual scope has no canonical identity");
    core::DisableHandoffAcceptedDTO accepted{
        request.owner_request_context.request_id,
        core::RequestReplayStatusDTO::kAcceptedNew,
        request.lifecycle_transaction_id,
        request.lifecycle_transaction_generation,
        request.controllable_component_id,
        scope_record.scope_resolution,
        scope_digest.value(),
        fence_id,
        core::ConfigurationFenceGeneration(next_generation),
        stop_acceptance.has_value()
            ? std::optional<core::StopId>(stop_acceptance->stop_id)
            : std::nullopt,
        stop_acceptance.has_value() ? std::optional<core::StopGeneration>(
                                          stop_acceptance->stop_generation)
                                    : std::nullopt};
    Require(accepted.matches(request),
            "constructed Disable-handoff acceptance is invalid");
    const auto accepted_bytes =
        core::detail::EncodeDisableHandoffAcceptedRecord(accepted);
    Transaction transaction(database_);
    Statement update(
        database_,
        "UPDATE kernel_apply_fences SET accepted_blob=? WHERE owner_kind="
        "'disable' AND request_owner_epoch=? AND request_id=? AND state=0 "
        "AND accepted_blob IS NULL");
    update.Blob(1, accepted_bytes);
    update.Text(2, request.owner_request_context.request_owner_epoch.value());
    update.Text(3, request.owner_request_context.request_id.value());
    Require(update.Step() == SQLITE_DONE && sqlite3_changes(database_) == 1,
            "durable Disable acceptance compare-and-swap failed");
    transaction.Commit();
    return accepted;
  }

  [[nodiscard]] core::DisableHandoffStatusDTO DisableHandoffStatus(
      const core::LifecycleTransactionId &lifecycle_transaction_id,
      const core::LifecycleTransactionGeneration
          lifecycle_transaction_generation) const {
    std::lock_guard lock(mutex_);
    return DisableHandoffStatusLocked(lifecycle_transaction_id,
                                      lifecycle_transaction_generation);
  }

  [[nodiscard]] core::DisableHandoffAttemptReconciliationDTO
  ReconcileDisableHandoffAttempt(
      const core::DisableHandoffRequestDTO &request) const {
    std::lock_guard lock(mutex_);
    Require(request.locally_valid(),
            "Disable-handoff reconciliation request is invalid");
    const auto encoded =
        core::detail::EncodeDisableHandoffRequestRecord(request);
    const auto row =
        ReadByRequest(request.owner_request_context.request_owner_epoch,
                      request.owner_request_context.request_id);
    if (!row.has_value()) {
      if (request.owner_request_context.request_owner_epoch ==
          config_.request_owner_epoch) {
        return {request, core::DisableHandoffAttemptDispositionDTO::kNotSeen,
                std::nullopt, std::nullopt, std::nullopt};
      }
      return {request, core::DisableHandoffAttemptDispositionDTO::kUncertain,
              std::nullopt, std::nullopt,
              core::ErrorDTO{
                  "kernel_disable_handoff_owner_replaced",
                  "replacement owner cannot prove the stale request was not "
                  "seen",
                  {}}};
    }
    if (row->owner_kind != DurableFenceOwnerKind::kDisable ||
        row->intent_bytes != encoded) {
      return {request, core::DisableHandoffAttemptDispositionDTO::kConflicting,
              std::nullopt, std::nullopt,
              core::ErrorDTO{
                  "kernel_disable_handoff_request_conflict",
                  "the durable request identity names another fence outcome",
                  {}}};
    }
    if (row->accepted_bytes.empty()) {
      return {
          request, core::DisableHandoffAttemptDispositionDTO::kUncertain,
          std::nullopt, std::nullopt,
          core::ErrorDTO{"kernel_disable_handoff_possible_effect",
                         "the durable prepared fence has no authoritative Stop "
                         "acceptance result",
                         {}}};
    }
    if (row->state != 0) {
      return {request, core::DisableHandoffAttemptDispositionDTO::kConflicting,
              std::nullopt, std::nullopt,
              core::ErrorDTO{
                  "kernel_disable_handoff_already_released",
                  "the exact handoff was accepted and its fence is released",
                  {}}};
    }
    const auto accepted =
        core::detail::DecodeDisableHandoffAcceptedRecord(row->accepted_bytes);
    const auto status =
        DisableHandoffStatusLocked(request.lifecycle_transaction_id,
                                   request.lifecycle_transaction_generation);
    core::DisableHandoffAttemptReconciliationDTO result{
        request, core::DisableHandoffAttemptDispositionDTO::kAcceptedExact,
        accepted, status, std::nullopt};
    Require(result.locally_valid(),
            "constructed Disable-handoff reconciliation is invalid");
    return result;
  }

  [[nodiscard]] core::DisableFenceReleasedDTO
  ReleaseDisableFence(const core::DisableFenceReleaseRequestDTO &request) {
    std::lock_guard lock(mutex_);
    Require(request.locally_valid() &&
                core::VerifyDisableHandoffAdmissionSnapshot(
                    request.current_admission_snapshot)
                    .ok(),
            "Disable-fence release request is invalid");
    Require(request.owner_request_context.request_owner_epoch ==
                config_.request_owner_epoch,
            "Disable-fence release names a stale Kernel owner epoch");
    const auto request_bytes =
        core::detail::EncodeDisableFenceReleaseRequestRecord(request);
    const auto row =
        ReadByFence(request.ready_handoff.configuration_fence_id,
                    request.ready_handoff.configuration_fence_generation);
    Require(row.has_value() &&
                row->owner_kind == DurableFenceOwnerKind::kDisable,
            "Disable-fence release identity is absent or foreign");
    if (row->state == 1) {
      Require(row->release_request_bytes == request_bytes &&
                  row->release_receipt_bytes.has_value(),
              "exact Disable-fence release replay conflicts with its receipt");
      auto released = core::detail::DecodeDisableFenceReleasedRecord(
          *row->release_receipt_bytes);
      released.replay_status = core::RequestReplayStatusDTO::kReplayedExact;
      return released;
    }
    Require(!row->accepted_bytes.empty() &&
                row->disable_fence_scope_bytes.has_value(),
            "Disable-fence release names an unresolved prepared handoff");
    const auto accepted =
        core::detail::DecodeDisableHandoffAcceptedRecord(row->accepted_bytes);
    const auto scope_record =
        core::detail::DecodeDisableHandoffFenceScopeRecord(
            *row->disable_fence_scope_bytes);
    const auto current =
        DisableHandoffStatusLocked(request.lifecycle_transaction_id,
                                   request.lifecycle_transaction_generation);
    Require(current == request.ready_handoff &&
                accepted.configuration_fence_id ==
                    current.configuration_fence_id &&
                accepted.configuration_fence_generation ==
                    current.configuration_fence_generation &&
                scope_record.scope == current.scope,
            "Disable-fence release conflicts with its ready handoff");
    const auto fresh = ObserveDisableHandoffAdmissionLocked(
        request.current_admission_snapshot.query);
    Require(fresh == request.current_admission_snapshot,
            "Disable-fence release admission facts changed before release");

    core::DisableFenceReleasedDTO released{
        request,
        core::RequestReplayStatusDTO::kAcceptedNew,
        config_.now_ns(),
        accepted.configuration_fence_id,
        accepted.configuration_fence_generation,
        accepted.component_scope_digest,
        Digest("manyforge.kernel-disable-fence-release-receipt.seed.v1")};
    released.receipt_digest =
        core::ContentDigest(content_identity::sha256_digest(
            core::detail::EncodeDisableFenceReleasedRecord(released)));
    Require(released.locally_valid(),
            "constructed Disable-fence release receipt is invalid");
    const auto receipt_bytes =
        core::detail::EncodeDisableFenceReleasedRecord(released);
    admission_gate_.ReleaseDisableFenceDurably(
        accepted.configuration_fence_id, scope_record.scope,
        [&](const ManagedKernelAdmissionObservation &observation) {
          Require(observation.operation_resource_generation ==
                          fresh.operation_resource_generation &&
                      observation.affected_queued_operation_count == 0U &&
                      observation.affected_active_operation_count == 0U &&
                      SelectCurrentConfigurations(
                          observation.active_control_configurations,
                          scope_record.scope) ==
                          fresh.current_active_control_configurations,
                  "Disable-fence release facts changed under the held fence");
          Transaction transaction(database_);
          Statement update(
              database_,
              "UPDATE kernel_apply_fences SET state=1,"
              "release_request_blob=?,release_receipt_blob=? WHERE "
              "owner_kind='disable' AND fence_id=? AND fence_generation=? "
              "AND state=0");
          update.Blob(1, request_bytes);
          update.Blob(2, receipt_bytes);
          update.Text(3, accepted.configuration_fence_id.value());
          update.Integer(4, accepted.configuration_fence_generation.value());
          Require(update.Step() == SQLITE_DONE &&
                      sqlite3_changes(database_) == 1,
                  "durable Disable-fence release compare-and-swap failed");
          transaction.Commit();
        });
    return released;
  }

  [[nodiscard]] core::DisableFenceReleaseAttemptReconciliationDTO
  ReconcileDisableFenceReleaseAttempt(
      const core::DisableFenceReleaseRequestDTO &request) const {
    std::lock_guard lock(mutex_);
    Require(request.locally_valid(),
            "Disable-fence release reconciliation request is invalid");
    const auto row =
        ReadByFence(request.ready_handoff.configuration_fence_id,
                    request.ready_handoff.configuration_fence_generation);
    if (!row.has_value()) {
      const auto disposition =
          request.owner_request_context.request_owner_epoch ==
                  config_.request_owner_epoch
              ? core::DisableFenceReleaseAttemptDispositionDTO::kConflicting
              : core::DisableFenceReleaseAttemptDispositionDTO::kUncertain;
      return {request, disposition, std::nullopt, std::nullopt,
              core::ErrorDTO{
                  "kernel_disable_fence_release_missing",
                  "the release fence is absent from the current durable owner",
                  {}}};
    }
    if (row->owner_kind != DurableFenceOwnerKind::kDisable) {
      return {request,
              core::DisableFenceReleaseAttemptDispositionDTO::kConflicting,
              std::nullopt, std::nullopt,
              core::ErrorDTO{"kernel_disable_fence_release_conflict",
                             "the release identity names a foreign fence",
                             {}}};
    }
    const auto encoded =
        core::detail::EncodeDisableFenceReleaseRequestRecord(request);
    if (row->state == 1 && row->release_request_bytes == encoded &&
        row->release_receipt_bytes.has_value()) {
      const auto receipt = core::detail::DecodeDisableFenceReleasedRecord(
          *row->release_receipt_bytes);
      core::DisableFenceReleaseAttemptReconciliationDTO result{
          request,
          core::DisableFenceReleaseAttemptDispositionDTO::kReleasedExact,
          receipt, std::nullopt, std::nullopt};
      Require(result.locally_valid(),
              "constructed exact Disable release reconciliation is invalid");
      return result;
    }
    if (row->state == 0 && !row->accepted_bytes.empty()) {
      const auto status =
          DisableHandoffStatusLocked(request.lifecycle_transaction_id,
                                     request.lifecycle_transaction_generation);
      if (status == request.ready_handoff) {
        core::DisableFenceReleaseAttemptReconciliationDTO result{
            request,
            core::DisableFenceReleaseAttemptDispositionDTO::kNotReleasedHeld,
            std::nullopt, status, std::nullopt};
        Require(result.locally_valid(),
                "constructed held Disable release reconciliation is invalid");
        return result;
      }
    }
    return {request,
            core::DisableFenceReleaseAttemptDispositionDTO::kConflicting,
            std::nullopt, std::nullopt,
            core::ErrorDTO{
                "kernel_disable_fence_release_conflict",
                "the durable release outcome conflicts with the exact request",
                {}}};
  }

private:
  struct DisableRuntimeCapture final {
    core::KernelDisableHandoffRuntimeSnapshotDTO runtime;
    ActivePublication active;
  };

  [[nodiscard]] DisableRuntimeCapture CaptureDisableRuntimeLocked(
      const core::DisableHandoffAdmissionQueryDTO &query) const {
    Require(disable_handoff_kernel_ != nullptr,
            "Disable-handoff Kernel is not bound");
    auto runtime =
        disable_handoff_kernel_->capture_disable_handoff_runtime_snapshot(
            query);
    if (!runtime.ok()) {
      Fail("Kernel Disable admission snapshot failed: " +
           runtime.error().message);
    }
    auto active = ReadActivePublication();
    Require(runtime.value().request_owner_epoch ==
                    config_.request_owner_epoch &&
                runtime.value().retained_bundle_id ==
                    active.receipt.request.candidate_retained_bundle_id &&
                runtime.value().graph_revision ==
                    active.receipt.request.candidate_graph_revision,
            "Kernel Disable runtime and active publication are not coherent");
    if (query.component_expectation.has_value() &&
        runtime.value().scope_resolution ==
            core::DisableHandoffScopeResolutionDTO::kExact &&
        (query.component_expectation->active_publication_generation !=
             active.receipt.publication_generation ||
         query.component_expectation->active_public_pointer_digest !=
             active.receipt.durable_public_pointer_content_digest)) {
      runtime.value().scope_resolution = core::
          DisableHandoffScopeResolutionDTO::kStaleButUnambiguouslyResolved;
    }
    return {std::move(runtime.value()), std::move(active)};
  }

  [[nodiscard]] DisableRuntimeCapture RevalidateDisableHandoffRequestLocked(
      const core::DisableHandoffRequestDTO &request) const {
    Require(disable_handoff_kernel_ != nullptr,
            "Disable-handoff Kernel is not bound");
    auto runtime =
        disable_handoff_kernel_->revalidate_disable_handoff_request(request);
    if (!runtime.ok()) {
      Fail("Kernel Disable request revalidation failed: " +
           runtime.error().message);
    }
    auto active = ReadActivePublication();
    Require(runtime.value().request_owner_epoch ==
                    config_.request_owner_epoch &&
                runtime.value().retained_bundle_id ==
                    active.receipt.request.candidate_retained_bundle_id &&
                runtime.value().graph_revision ==
                    active.receipt.request.candidate_graph_revision,
            "Kernel Disable runtime and active publication are not coherent");
    if (runtime.value().scope_resolution ==
            core::DisableHandoffScopeResolutionDTO::kExact &&
        (request.expected_active_publication_generation !=
             active.receipt.publication_generation ||
         request.expected_active_public_pointer_digest !=
             active.receipt.durable_public_pointer_content_digest)) {
      runtime.value().scope_resolution = core::
          DisableHandoffScopeResolutionDTO::kStaleButUnambiguouslyResolved;
    }
    return {std::move(runtime.value()), std::move(active)};
  }

  [[nodiscard]] core::DisableHandoffAdmissionSnapshotDTO
  ObserveDisableHandoffAdmissionLocked(
      const core::DisableHandoffAdmissionQueryDTO &query) const {
    Require(query.locally_valid(),
            "Disable-handoff admission query is invalid");
    auto capture = CaptureDisableRuntimeLocked(query);
    core::DisableHandoffAdmissionSnapshotDTO snapshot{
        query,
        config_.request_owner_epoch,
        capture.runtime.scope_resolution,
        capture.runtime.current_scope,
        capture.active.receipt.publication_generation,
        capture.active.receipt.durable_public_pointer_content_digest,
        capture.runtime.retained_bundle_id,
        capture.runtime.graph_revision,
        capture.runtime.operation_resource_generation,
        core::ConfigurationFenceGeneration(ReadFenceGeneration()),
        capture.runtime.current_active_control_configurations,
        Digest("manyforge.kernel-disable-admission-snapshot.seed.v1")};
    const auto digest =
        core::ComputeDisableHandoffAdmissionSnapshotDigest(snapshot);
    Require(digest.ok(),
            "constructed Disable admission snapshot is not canonical");
    snapshot.snapshot_digest = digest.value();
    Require(core::VerifyDisableHandoffAdmissionSnapshot(snapshot).ok(),
            "constructed Disable admission snapshot is invalid");
    return snapshot;
  }

  [[nodiscard]] core::DisableHandoffStatusDTO DisableHandoffStatusLocked(
      const core::LifecycleTransactionId &lifecycle_transaction_id,
      const core::LifecycleTransactionGeneration
          lifecycle_transaction_generation) const {
    const auto row = ReadByDisableOwner(lifecycle_transaction_id,
                                        lifecycle_transaction_generation);
    Require(row.has_value() &&
                row->owner_kind == DurableFenceOwnerKind::kDisable &&
                row->state == 0 && !row->accepted_bytes.empty() &&
                row->disable_fence_scope_bytes.has_value(),
            "Disable-handoff status is absent, unresolved, or released");
    const auto request =
        core::detail::DecodeDisableHandoffRequestRecord(row->intent_bytes);
    const auto accepted =
        core::detail::DecodeDisableHandoffAcceptedRecord(row->accepted_bytes);
    const auto scope_record =
        core::detail::DecodeDisableHandoffFenceScopeRecord(
            *row->disable_fence_scope_bytes);
    Require(accepted.matches(request) &&
                accepted.scope_resolution == scope_record.scope_resolution,
            "durable Disable-handoff status conflicts with its request");

    auto state = core::DisableHandoffStateDTO::kReady;
    std::optional<core::StopCertificateRefDTO> certificate;
    std::optional<core::ErrorDTO> status_error;
    if (accepted.stop_id.has_value()) {
      Require(disable_handoff_kernel_ != nullptr,
              "Disable-handoff Kernel is not bound for status observation");
      const auto stop =
          disable_handoff_kernel_->get_stop_status(*accepted.stop_id);
      if (!stop.ok()) {
        Fail("Disable-handoff Stop status is unavailable: " +
             stop.error().message);
      }
      switch (stop.value().state) {
      case core::StopStatusStateDTO::kAccepted:
      case core::StopStatusStateDTO::kStopping:
      case core::StopStatusStateDTO::kCertifying:
        state = core::DisableHandoffStateDTO::kStopping;
        break;
      case core::StopStatusStateDTO::kSucceeded:
        Require(stop.value().stop_certificate.has_value(),
                "successful Disable Stop lacks its certificate reference");
        state = core::DisableHandoffStateDTO::kReady;
        certificate = stop.value().stop_certificate;
        break;
      case core::StopStatusStateDTO::kPartial:
      case core::StopStatusStateDTO::kFailed:
        state = core::DisableHandoffStateDTO::kFailed;
        status_error = core::ErrorDTO{
            "kernel_disable_stop_not_certified",
            "Disable containment did not produce a successful Stop "
            "certificate",
            {}};
        break;
      }
    }
    const core::ConfigurationFenceHandleDTO handle{
        accepted.configuration_fence_id,
        accepted.configuration_fence_generation,
        accepted.lifecycle_transaction_id,
        accepted.lifecycle_transaction_generation,
        accepted.controllable_component_id,
        scope_record.scope,
        "component_lifecycle_disable_handoff",
        core::ConfigurationFenceState::kHeld};
    core::DisableHandoffStatusDTO status{
        accepted.lifecycle_transaction_id,
        accepted.lifecycle_transaction_generation,
        accepted.controllable_component_id,
        accepted.scope_resolution,
        accepted.component_scope_digest,
        accepted.configuration_fence_id,
        accepted.configuration_fence_generation,
        state,
        scope_record.scope,
        handle,
        certificate,
        {},
        status_error};
    Require(status.locally_valid(),
            "constructed Disable-handoff status is invalid");
    return status;
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

  [[nodiscard]] static ManagedKernelApplyFenceOwnerConfig
  ValidateConfig(ManagedKernelApplyFenceOwnerConfig config) {
    config.ledger_path = ValidateLedgerPath(std::move(config.ledger_path));
    config.manager_state_root = ExistingPlainDirectory(
        std::move(config.manager_state_root), "Manager state root");
    config.retained_package_root = ExistingPlainDirectory(
        std::move(config.retained_package_root), "retained-package root");
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
      Fail("durable ledger read-only preflight failed: " +
           std::string(error.what()));
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
          "('apply','route','disable')),owner_id TEXT NOT NULL,"
          "owner_generation INTEGER NOT NULL CHECK("
          "owner_generation>0),request_id TEXT NOT NULL,request_owner_epoch "
          "TEXT NOT NULL,intent_blob BLOB NOT NULL,accepted_blob BLOB,"
          "disable_fence_scope_blob BLOB,"
          "fence_id TEXT NOT NULL,fence_generation INTEGER NOT NULL CHECK("
          "fence_generation>0),state INTEGER NOT NULL CHECK(state IN (0,1)),"
          "release_request_blob BLOB,release_receipt_blob BLOB,"
          "PRIMARY KEY(request_owner_epoch,request_id),"
          "UNIQUE(owner_kind,owner_id,owner_generation),UNIQUE(fence_id),"
          "UNIQUE(fence_generation),"
          "CHECK((owner_kind IN ('apply','route') AND accepted_blob IS NOT "
          "NULL AND disable_fence_scope_blob IS NULL) OR (owner_kind='disable' "
          "AND disable_fence_scope_blob IS NOT NULL)),"
          "CHECK(state=0 OR accepted_blob IS NOT NULL),"
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
                        "disable_fence_scope_blob,fence_id FROM "
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
      } else {
        Require(owner_kind == ToString(DurableFenceOwnerKind::kDisable),
                "durable fence has an unknown owner kind");
        const auto request = core::detail::DecodeDisableHandoffRequestRecord(
            statement.ColumnBlob(1));
        const auto scope_record =
            core::detail::DecodeDisableHandoffFenceScopeRecord(
                statement.ColumnBlob(3));
        Require(
            ScopeMembershipIsSubset(request.affected_scope,
                                    scope_record.scope) &&
                (request.scope_resolution !=
                     core::DisableHandoffScopeResolutionDTO::kPromotedToFull ||
                 scope_record.scope_resolution ==
                     core::DisableHandoffScopeResolutionDTO::kPromotedToFull),
            "prepared Disable fence narrows its durable request");
        const auto fence_id =
            core::ConfigurationFenceId(statement.ColumnText(4));
        const auto accepted_bytes = statement.OptionalColumnBlob(2);
        if (accepted_bytes.has_value()) {
          const auto accepted =
              core::detail::DecodeDisableHandoffAcceptedRecord(*accepted_bytes);
          const auto scope_digest =
              core::ComputeDisableHandoffScopeDigest(scope_record.scope);
          Require(scope_digest.ok() && accepted.matches(request) &&
                      accepted.scope_resolution ==
                          scope_record.scope_resolution &&
                      accepted.component_scope_digest == scope_digest.value() &&
                      accepted.configuration_fence_id == fence_id,
                  "durable Disable acceptance conflicts with its fence scope");
        }
        admission_gate_.RestoreFence(fence_id, scope_record.scope);
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
                        "owner_kind,intent_blob,accepted_blob,disable_fence_"
                        "scope_blob,state,release_request_blob,"
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
        "owner_kind,intent_blob,accepted_blob,disable_fence_scope_blob,state,"
        "release_request_blob,"
        "release_receipt_blob FROM kernel_apply_fences WHERE fence_id=? AND "
        "fence_generation=?");
    statement.Text(1, fence_id.value());
    statement.Integer(2, generation.value());
    return ReadOptionalRow(statement);
  }

  [[nodiscard]] std::optional<DurableFenceRow> ReadByDisableOwner(
      const core::LifecycleTransactionId &transaction_id,
      const core::LifecycleTransactionGeneration transaction_generation) const {
    Statement statement(
        database_,
        "SELECT owner_kind,intent_blob,accepted_blob,disable_fence_scope_blob,"
        "state,release_request_blob,release_receipt_blob FROM "
        "kernel_apply_fences WHERE owner_kind='disable' AND owner_id=? AND "
        "owner_generation=?");
    statement.Text(1, transaction_id.value());
    statement.Integer(2, transaction_generation.value());
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
                owner_kind == ToString(DurableFenceOwnerKind::kDisable),
            "durable fence owner kind is invalid");
    const auto kind = owner_kind == ToString(DurableFenceOwnerKind::kApply)
                          ? DurableFenceOwnerKind::kApply
                      : owner_kind == ToString(DurableFenceOwnerKind::kRoute)
                          ? DurableFenceOwnerKind::kRoute
                          : DurableFenceOwnerKind::kDisable;
    DurableFenceRow row{kind,
                        statement.ColumnBlob(1),
                        statement.OptionalColumnBlob(2).value_or(""),
                        statement.OptionalColumnBlob(3),
                        static_cast<int>(statement.ColumnInteger(4)),
                        statement.OptionalColumnBlob(5),
                        statement.OptionalColumnBlob(6)};
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
                   "owner_kind,intent_blob,accepted_blob,disable_fence_scope_"
                   "blob,state,release_request_blob,"
                   "release_receipt_blob FROM kernel_apply_fences ORDER BY "
                   "fence_generation");
    while (statement.Step() == SQLITE_ROW) {
      AppendField(bytes, statement.ColumnText(0));
      AppendField(bytes, statement.ColumnBlob(1));
      AppendField(bytes, statement.OptionalColumnBlob(2).value_or(""));
      AppendField(bytes, statement.OptionalColumnBlob(3).value_or(""));
      AppendField(bytes, std::to_string(statement.ColumnInteger(4)));
      AppendField(bytes, statement.OptionalColumnBlob(5).value_or(""));
      AppendField(bytes, statement.OptionalColumnBlob(6).value_or(""));
    }
    return core::ContentDigest(content_identity::sha256_digest(bytes));
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
    Require(bundle != nullptr &&
                bundle->value().cell_graph.cell_id == config_.cell_id &&
                bundle->value().cell_graph.graph_revision ==
                    result.value().request.candidate_graph_revision,
            "active publication does not resolve to its exact retained "
            "bundle");
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
  QueryStatusLocked(const core::ConfigurationFenceStatusQueryDTO &query) const {
    Require(query.valid(), "fence status query is invalid");
    const auto row = ReadByFence(query.configuration_fence_id,
                                 query.configuration_fence_generation);
    Require(row.has_value(), "fence status identity is unknown");
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
  core::ResourceAddressedExecutionKernel *disable_handoff_kernel_{};
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

void ManagedKernelApplyFenceOwner::BindDisableHandoffKernel(
    core::ResourceAddressedExecutionKernel &kernel) {
  impl_->BindDisableHandoffKernel(kernel);
}

core::Result<core::DisableHandoffAdmissionSnapshotDTO>
ManagedKernelApplyFenceOwner::ObserveDisableHandoffAdmission(
    const core::DisableHandoffAdmissionQueryDTO &query) const {
  return InvokeDisableHandoffPort<core::DisableHandoffAdmissionSnapshotDTO>(
      [&] { return impl_->ObserveDisableHandoffAdmission(query); });
}

core::Result<core::DisableHandoffAcceptedDTO>
ManagedKernelApplyFenceOwner::BeginDisableHandoff(
    const core::DisableHandoffRequestDTO &request) {
  return InvokeDisableHandoffPort<core::DisableHandoffAcceptedDTO>(
      [&] { return impl_->BeginDisableHandoff(request); });
}

core::Result<core::DisableHandoffStatusDTO>
ManagedKernelApplyFenceOwner::DisableHandoffStatus(
    const core::LifecycleTransactionId &lifecycle_transaction_id,
    const core::LifecycleTransactionGeneration
        lifecycle_transaction_generation) {
  return InvokeDisableHandoffPort<core::DisableHandoffStatusDTO>([&] {
    return impl_->DisableHandoffStatus(lifecycle_transaction_id,
                                       lifecycle_transaction_generation);
  });
}

core::Result<core::DisableHandoffAttemptReconciliationDTO>
ManagedKernelApplyFenceOwner::ReconcileDisableHandoffAttempt(
    const core::DisableHandoffRequestDTO &request) {
  return InvokeDisableHandoffPort<core::DisableHandoffAttemptReconciliationDTO>(
      [&] { return impl_->ReconcileDisableHandoffAttempt(request); });
}

core::Result<core::DisableFenceReleasedDTO>
ManagedKernelApplyFenceOwner::ReleaseDisableFence(
    const core::DisableFenceReleaseRequestDTO &request) {
  return InvokeDisableHandoffPort<core::DisableFenceReleasedDTO>(
      [&] { return impl_->ReleaseDisableFence(request); });
}

core::Result<core::DisableFenceReleaseAttemptReconciliationDTO>
ManagedKernelApplyFenceOwner::ReconcileDisableFenceReleaseAttempt(
    const core::DisableFenceReleaseRequestDTO &request) {
  return InvokeDisableHandoffPort<
      core::DisableFenceReleaseAttemptReconciliationDTO>(
      [&] { return impl_->ReconcileDisableFenceReleaseAttempt(request); });
}

} // namespace manyforge::roboplan_backend
