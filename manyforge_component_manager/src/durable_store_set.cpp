#include "manyforge_component_manager/durable_store_set.hpp"

#include <manyforge_persistence/sqlite_read_only_preflight.hpp>

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

#include <manyforge_deployment/canonical_empty_admission.hpp>

namespace manyforge::component_manager {
namespace {

struct RetainedAdmissionIdentity final {
  core::ContentDigest admission_digest;
  core::CellId cell_id;
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
};

RetainedAdmissionIdentity IdentityFromAdmission(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  const auto& bundle = admission.retained_bundle();
  const auto& value = bundle.value();
  if (admission.api_version() !=
          deployment::kCanonicalEmptyAdmissionApiVersion ||
      admission.admission_digest().empty() ||
      admission.canonical_bytes().empty() ||
      !deployment::verify_retained_bundle_identity(bundle) ||
      admission.cell_id().empty() ||
      admission.retained_bundle_id().empty() ||
      admission.graph_revision().empty() ||
      bundle.retained_bundle_id() != admission.retained_bundle_id() ||
      value.retained_bundle_id != admission.retained_bundle_id() ||
      value.cell_graph.cell_id != admission.cell_id() ||
      value.cell_graph.graph_revision != admission.graph_revision()) {
    throw std::invalid_argument(
        "manager durable stores require one internally consistent "
        "canonical-empty admission");
  }
  return {
      admission.admission_digest(),
      admission.cell_id(),
      admission.retained_bundle_id(),
      admission.graph_revision(),
  };
}

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
  try {
    bool initialized = false;
    const bool exists = persistence::PreflightExistingSqliteReadOnly(
        path,
        {
            64U * 1024U * 1024U,
            64U * 1024U * 1024U,
        },
        [&initialized](sqlite3* database) {
          initialized =
              ReadSqliteScalar(
                  database,
                  "SELECT COUNT(*) FROM sqlite_master "
                  "WHERE name NOT LIKE 'sqlite_%'") != 0 ||
              ReadSqliteScalar(database, "PRAGMA application_id") != 0 ||
              ReadSqliteScalar(database, "PRAGMA user_version") != 0;
        });
    return exists && initialized;
  } catch (...) {
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
  Impl(
      std::filesystem::path stable_state_directory,
      RetainedAdmissionIdentity admission_identity)
      : stable_state_directory_(
            ValidateStableStateDirectory(
                std::move(stable_state_directory))),
        admission_digest_(
            std::move(admission_identity.admission_digest)),
        cell_id_(std::move(admission_identity.cell_id)),
        retained_bundle_id_(
            std::move(admission_identity.retained_bundle_id)),
        graph_revision_(std::move(admission_identity.graph_revision)) {
    StableStateDirectoryLock startup_lock(stable_state_directory_);
    const auto journal_path =
        stable_state_directory_ /
        std::string(kManagerBootstrapJournalFilename);
    const auto publication_path =
        stable_state_directory_ /
        std::string(kActiveBundlePublicationStoreFilename);
    const auto apply_path =
        stable_state_directory_ /
        std::string(kManagerApplyJournalFilename);
    const bool journal_requires_validation =
        RequiresValidationBeforeInitialization(journal_path);
    const bool publication_requires_validation =
        RequiresValidationBeforeInitialization(publication_path);
    const bool apply_requires_validation =
        RequiresValidationBeforeInitialization(apply_path);

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
    const auto open_apply = [&] {
      apply_journal_ = std::make_unique<SqliteManagerApplyJournal>(
          apply_path, cell_id_);
    };

    // Validate every initialized peer before initializing any missing or
    // schema-empty path. A foreign-cell initialized store must not make this
    // failed startup create another store under the requested (wrong) cell.
    if (journal_requires_validation) {
      open_journal();
    }
    if (publication_requires_validation) {
      open_publication();
    }
    if (apply_requires_validation) {
      open_apply();
    }
    if (!journal_requires_validation) {
      open_journal();
    }
    if (!publication_requires_validation) {
      open_publication();
    }
    if (!apply_requires_validation) {
      open_apply();
    }

    const auto journal_report = bootstrap_journal_->VerifyIntegrity();
    const auto publication_report =
        active_bundle_publication_store_->VerifyIntegrity();
    const auto apply_report = apply_journal_->VerifyIntegrity();
    if (journal_report.cell_id != cell_id_ ||
        publication_report.cell_id != cell_id_ ||
        apply_report.cell_id != cell_id_) {
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
  core::ContentDigest admission_digest_;
  core::CellId cell_id_;
  core::RetainedBundleId retained_bundle_id_;
  core::GraphRevision graph_revision_;
  std::unique_ptr<SqliteManagerBootstrapJournal> bootstrap_journal_;
  std::unique_ptr<SqliteActiveBundlePublicationStore>
      active_bundle_publication_store_;
  std::unique_ptr<SqliteManagerApplyJournal> apply_journal_;
};

ExternallyProvisionedManagerStores::ExternallyProvisionedManagerStores(
    std::filesystem::path stable_state_directory,
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission)
    : impl_(std::make_unique<Impl>(
          std::move(stable_state_directory),
          IdentityFromAdmission(admission))) {}

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

bool ExternallyProvisionedManagerStores::matches_admission(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission)
    const noexcept {
  return impl_->admission_digest_ == admission.admission_digest() &&
         impl_->cell_id_ == admission.cell_id() &&
         impl_->retained_bundle_id_ == admission.retained_bundle_id() &&
         impl_->graph_revision_ == admission.graph_revision();
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

SqliteManagerApplyJournal&
ExternallyProvisionedManagerStores::apply_journal() noexcept {
  return *impl_->apply_journal_;
}

ManagerDurableStoreStartupInventory
ExternallyProvisionedManagerStores::StartupInventory() const {
  const auto bootstrap = impl_->bootstrap_journal_->VerifyIntegrity();
  const auto publication =
      impl_->active_bundle_publication_store_->VerifyIntegrity();
  const auto apply = impl_->apply_journal_->VerifyIntegrity();
  if (bootstrap.cell_id != impl_->cell_id_ ||
      publication.cell_id != impl_->cell_id_ ||
      apply.cell_id != impl_->cell_id_) {
    throw ManagerBootstrapJournalIntegrityError(
        "manager durable-store inventory cell identity changed");
  }
  return {bootstrap, publication, apply};
}

}  // namespace manyforge::component_manager
