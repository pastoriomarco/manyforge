#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>

struct sqlite3;

namespace manyforge::persistence {

class SqliteReadOnlyPreflightError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct SqliteReadOnlyPreflightLimits final {
  std::uintmax_t maximum_main_database_bytes;
  std::uintmax_t maximum_wal_bytes;
};

/// Run validation against an existing SQLite store without changing its
/// durable database or WAL content.
///
/// A clean at-rest WAL database is opened as immutable so SQLite cannot
/// create sidecars merely to inspect it. A live WAL database with both
/// sidecars is opened read-only; SQLite may update transient coordination
/// bytes in the already-existing SHM file. Crash residue that has a WAL but
/// no SHM is copied into a bounded private directory so WAL-index recovery
/// occurs only against the copy. Missing paths return false. The validation
/// callback is not invoked for a missing path.
bool PreflightExistingSqliteReadOnly(
    const std::filesystem::path& database_path,
    SqliteReadOnlyPreflightLimits limits,
    const std::function<void(sqlite3*)>& validate);

}  // namespace manyforge::persistence
