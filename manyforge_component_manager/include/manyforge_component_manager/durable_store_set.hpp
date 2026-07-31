#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/manager_bootstrap_journal.hpp>

namespace manyforge::deployment {
class CanonicalEmptyRetainedBundleAdmission;
}

namespace manyforge::component_manager {

inline constexpr std::string_view kManagerBootstrapJournalFilename =
    "manager-bootstrap-journal.sqlite3";
inline constexpr std::string_view kActiveBundlePublicationStoreFilename =
    "active-bundle-publication.sqlite3";

struct ManagerDurableStoreStartupInventory final {
  ManagerBootstrapJournalIntegrityReport bootstrap_journal;
  ActiveBundlePublicationStoreIntegrityReport active_bundle_publication;
};

/// External composition-root owner for the two independently durable stores.
///
/// The supplied directory must already exist and be owned by deployment
/// bootstrap. The cell identity is derived only from a sealed, verified
/// canonical-empty admission; callers cannot inject a parallel CellId. This
/// object opens both stores with that exact identity and fsyncs the directory
/// after successful initialization. It does not make the two SQLite databases
/// one transaction, infer cross-store recovery, start the Manager process, or
/// invoke any Manager state-machine operation.
class ExternallyProvisionedManagerStores final {
 public:
  ExternallyProvisionedManagerStores(
      std::filesystem::path stable_state_directory,
      const manyforge::deployment::
          CanonicalEmptyRetainedBundleAdmission& admission);
  ~ExternallyProvisionedManagerStores();

  ExternallyProvisionedManagerStores(
      ExternallyProvisionedManagerStores&&) noexcept;
  ExternallyProvisionedManagerStores& operator=(
      ExternallyProvisionedManagerStores&&) noexcept;
  ExternallyProvisionedManagerStores(
      const ExternallyProvisionedManagerStores&) = delete;
  ExternallyProvisionedManagerStores& operator=(
      const ExternallyProvisionedManagerStores&) = delete;

  [[nodiscard]] const std::filesystem::path& stable_state_directory()
      const noexcept;
  [[nodiscard]] const manyforge::core::CellId& cell_id() const noexcept;
  [[nodiscard]] SqliteManagerBootstrapJournal& bootstrap_journal() noexcept;
  [[nodiscard]] SqliteActiveBundlePublicationStore&
  active_bundle_publication_store() noexcept;
  [[nodiscard]] ManagerDurableStoreStartupInventory StartupInventory() const;

 private:
  /// Private coordinator invariant for the exact sealed admission that opened
  /// this in-memory store set. SQLite remains keyed by CellId, but the
  /// coordinator cannot substitute another package for this process.
  [[nodiscard]] bool matches_admission(
      const manyforge::deployment::
          CanonicalEmptyRetainedBundleAdmission& admission) const noexcept;

  class Impl;
  std::unique_ptr<Impl> impl_;

  friend class CanonicalEmptyBootstrapCoordinator;
};

}  // namespace manyforge::component_manager
