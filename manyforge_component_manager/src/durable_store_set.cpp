#include "manyforge_component_manager/durable_store_set.hpp"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace manyforge::component_manager {
namespace {

std::filesystem::path ValidateStableStateDirectory(
    std::filesystem::path directory) {
  if (directory.empty()) {
    throw std::invalid_argument(
        "manager stable state directory cannot be empty");
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(directory, error);
  if (error) {
    throw std::invalid_argument(
        "cannot inspect manager stable state directory");
  }
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_directory(status)) {
    throw std::invalid_argument(
        "manager stable state directory must be a non-symlink directory");
  }
  return directory;
}

class StableStateDirectoryLock final {
 public:
  explicit StableStateDirectoryLock(
      const std::filesystem::path& directory) {
    const auto lock_path =
        directory / ".manager-durable-stores.lock";
    descriptor_ = open(
        lock_path.c_str(),
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (descriptor_ < 0) {
      throw ManagerBootstrapJournalError(
          "cannot open manager durable-store startup lock: " +
          std::string(std::strerror(errno)));
    }
    struct stat status {};
    if (fstat(descriptor_, &status) != 0 || !S_ISREG(status.st_mode)) {
      const auto detail = std::string(std::strerror(errno));
      close(descriptor_);
      descriptor_ = -1;
      throw ManagerBootstrapJournalError(
          "manager durable-store startup lock is not a regular file: " +
          detail);
    }
    while (flock(descriptor_, LOCK_EX) != 0) {
      if (errno == EINTR) {
        continue;
      }
      const auto detail = std::string(std::strerror(errno));
      close(descriptor_);
      descriptor_ = -1;
      throw ManagerBootstrapJournalError(
          "cannot acquire manager durable-store startup lock: " + detail);
    }
  }

  ~StableStateDirectoryLock() {
    if (descriptor_ >= 0) {
      flock(descriptor_, LOCK_UN);
      close(descriptor_);
    }
  }

  StableStateDirectoryLock(const StableStateDirectoryLock&) = delete;
  StableStateDirectoryLock& operator=(
      const StableStateDirectoryLock&) = delete;

 private:
  int descriptor_{-1};
};

std::int64_t ReadSqliteScalar(
    sqlite3* database,
    const std::string_view sql) {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(
          database,
          std::string(sql).c_str(),
          -1,
          &statement,
          nullptr) != SQLITE_OK) {
    if (statement != nullptr) {
      sqlite3_finalize(statement);
    }
    throw std::runtime_error("cannot inspect SQLite store preflight");
  }
  const auto step = sqlite3_step(statement);
  if (step != SQLITE_ROW ||
      sqlite3_column_type(statement, 0) != SQLITE_INTEGER) {
    sqlite3_finalize(statement);
    throw std::runtime_error("SQLite store preflight row is invalid");
  }
  const auto value = sqlite3_column_int64(statement, 0);
  if (sqlite3_step(statement) != SQLITE_DONE ||
      sqlite3_finalize(statement) != SQLITE_OK) {
    throw std::runtime_error(
        "SQLite store preflight result is invalid");
  }
  return value;
}

bool RequiresValidationBeforeInitialization(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::make_error_code(std::errc::no_such_file_or_directory)) {
    return false;
  }
  if (error) {
    throw std::invalid_argument(
        "cannot inspect manager durable-store path");
  }
  if (status.type() == std::filesystem::file_type::not_found) {
    return false;
  }
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    return true;
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return true;
  }
  if (size == 0U) {
    return false;
  }

  sqlite3* database = nullptr;
  const auto open_result = sqlite3_open_v2(
      path.string().c_str(),
      &database,
      SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX |
          SQLITE_OPEN_NOFOLLOW,
      nullptr);
  if (open_result != SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return true;
  }
  try {
    const bool initialized =
        ReadSqliteScalar(
            database,
            "SELECT COUNT(*) FROM sqlite_master "
            "WHERE name NOT LIKE 'sqlite_%'") != 0 ||
        ReadSqliteScalar(database, "PRAGMA application_id") != 0 ||
        ReadSqliteScalar(database, "PRAGMA user_version") != 0;
    if (sqlite3_close(database) != SQLITE_OK) {
      throw std::runtime_error(
          "cannot close SQLite store preflight");
    }
    return initialized;
  } catch (...) {
    sqlite3_close(database);
    return true;
  }
}

void FsyncDirectory(const std::filesystem::path& directory) {
  const int descriptor = open(
      directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    throw ManagerBootstrapJournalError(
        "cannot open manager stable state directory for fsync: " +
        std::string(std::strerror(errno)));
  }
  if (fsync(descriptor) != 0) {
    const auto detail = std::string(std::strerror(errno));
    close(descriptor);
    throw ManagerBootstrapJournalError(
        "cannot fsync manager stable state directory: " + detail);
  }
  if (close(descriptor) != 0) {
    throw ManagerBootstrapJournalError(
        "cannot close manager stable state directory after fsync: " +
        std::string(std::strerror(errno)));
  }
}

}  // namespace

class ExternallyProvisionedManagerStores::Impl final {
 public:
  Impl(std::filesystem::path stable_state_directory, core::CellId cell_id)
      : stable_state_directory_(
            ValidateStableStateDirectory(
                std::move(stable_state_directory))),
        cell_id_(std::move(cell_id)) {
    StableStateDirectoryLock startup_lock(stable_state_directory_);
    const auto journal_path =
        stable_state_directory_ /
        std::string(kManagerBootstrapJournalFilename);
    const auto publication_path =
        stable_state_directory_ /
        std::string(kActiveBundlePublicationStoreFilename);
    const bool journal_requires_validation =
        RequiresValidationBeforeInitialization(journal_path);
    const bool publication_requires_validation =
        RequiresValidationBeforeInitialization(publication_path);

    const auto open_journal = [&] {
      bootstrap_journal_ =
          std::make_unique<SqliteManagerBootstrapJournal>(
              journal_path, cell_id_);
    };
    const auto open_publication = [&] {
      active_bundle_publication_store_ =
          std::make_unique<SqliteActiveBundlePublicationStore>(
              publication_path, cell_id_);
    };

    // Validate the only initialized peer first. A missing or schema-empty
    // path has no CellId yet; a foreign-cell initialized store must not cause
    // this failed startup to initialize that peer with the requested (wrong)
    // cell.
    if (publication_requires_validation &&
        !journal_requires_validation) {
      open_publication();
      open_journal();
    } else {
      open_journal();
      open_publication();
    }

    const auto journal_report = bootstrap_journal_->VerifyIntegrity();
    const auto publication_report =
        active_bundle_publication_store_->VerifyIntegrity();
    if (journal_report.cell_id != cell_id_ ||
        publication_report.cell_id != cell_id_) {
      throw ManagerBootstrapJournalIntegrityError(
          "manager durable stores do not share the configured cell identity");
    }

    // SQLite FULL durability covers database/WAL writes. The external
    // composition root additionally persists any newly created directory
    // entries before it exposes the store set. Repeating this on reopen is
    // harmless and avoids a racy pre-open existence inference.
    FsyncDirectory(stable_state_directory_);
  }

  std::filesystem::path stable_state_directory_;
  core::CellId cell_id_;
  std::unique_ptr<SqliteManagerBootstrapJournal> bootstrap_journal_;
  std::unique_ptr<SqliteActiveBundlePublicationStore>
      active_bundle_publication_store_;
};

ExternallyProvisionedManagerStores::ExternallyProvisionedManagerStores(
    std::filesystem::path stable_state_directory,
    core::CellId cell_id)
    : impl_(std::make_unique<Impl>(
          std::move(stable_state_directory), std::move(cell_id))) {}

ExternallyProvisionedManagerStores::~ExternallyProvisionedManagerStores() =
    default;

ExternallyProvisionedManagerStores::ExternallyProvisionedManagerStores(
    ExternallyProvisionedManagerStores&&) noexcept = default;

ExternallyProvisionedManagerStores&
ExternallyProvisionedManagerStores::operator=(
    ExternallyProvisionedManagerStores&&) noexcept = default;

const std::filesystem::path&
ExternallyProvisionedManagerStores::stable_state_directory() const noexcept {
  return impl_->stable_state_directory_;
}

const core::CellId&
ExternallyProvisionedManagerStores::cell_id() const noexcept {
  return impl_->cell_id_;
}

SqliteManagerBootstrapJournal&
ExternallyProvisionedManagerStores::bootstrap_journal() noexcept {
  return *impl_->bootstrap_journal_;
}

SqliteActiveBundlePublicationStore&
ExternallyProvisionedManagerStores::
    active_bundle_publication_store() noexcept {
  return *impl_->active_bundle_publication_store_;
}

ManagerDurableStoreStartupInventory
ExternallyProvisionedManagerStores::StartupInventory() const {
  const auto bootstrap = impl_->bootstrap_journal_->VerifyIntegrity();
  const auto publication =
      impl_->active_bundle_publication_store_->VerifyIntegrity();
  if (bootstrap.cell_id != impl_->cell_id_ ||
      publication.cell_id != impl_->cell_id_) {
    throw ManagerBootstrapJournalIntegrityError(
        "manager durable-store inventory cell identity changed");
  }
  return {bootstrap, publication};
}

}  // namespace manyforge::component_manager
