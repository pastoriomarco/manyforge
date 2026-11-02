#include <manyforge_persistence/sqlite_read_only_preflight.hpp>

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace manyforge::persistence {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() /
         "manyforge-sqlite-preflight-XXXXXX")
            .string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const auto* created = mkdtemp(buffer.data());
    if (created == nullptr) {
      throw SqliteReadOnlyPreflightError(
          "cannot create private SQLite preflight directory: " +
          std::string(std::strerror(errno)));
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

class FileDescriptor final {
 public:
  explicit FileDescriptor(const int descriptor) : descriptor_(descriptor) {}
  ~FileDescriptor() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  [[nodiscard]] int get() const noexcept {
    return descriptor_;
  }

 private:
  int descriptor_;
};

struct FileStatus final {
  dev_t device;
  ino_t inode;
  off_t size;
  timespec modified;
  timespec changed;
};

FileStatus StatusOf(const int descriptor, const std::string_view label) {
  struct stat status {};
  if (fstat(descriptor, &status) != 0) {
    throw SqliteReadOnlyPreflightError(
        "cannot inspect " + std::string(label) + ": " +
        std::string(std::strerror(errno)));
  }
  if (!S_ISREG(status.st_mode) || status.st_size < 0) {
    throw SqliteReadOnlyPreflightError(
        std::string(label) + " must be a regular file");
  }
  return {
      status.st_dev,
      status.st_ino,
      status.st_size,
      status.st_mtim,
      status.st_ctim,
  };
}

bool SameStatus(const FileStatus& lhs, const FileStatus& rhs) {
  return lhs.device == rhs.device && lhs.inode == rhs.inode &&
      lhs.size == rhs.size &&
      lhs.modified.tv_sec == rhs.modified.tv_sec &&
      lhs.modified.tv_nsec == rhs.modified.tv_nsec &&
      lhs.changed.tv_sec == rhs.changed.tv_sec &&
      lhs.changed.tv_nsec == rhs.changed.tv_nsec;
}

void CopyStableRegularFile(
    const std::filesystem::path& source_path,
    const std::filesystem::path& destination_path,
    const std::uintmax_t maximum_bytes,
    const std::string_view label) {
  FileDescriptor source(open(
      source_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (source.get() < 0) {
    throw SqliteReadOnlyPreflightError(
        "cannot open " + std::string(label) + ": " +
        std::string(std::strerror(errno)));
  }
  const auto before = StatusOf(source.get(), label);
  if (static_cast<std::uintmax_t>(before.size) > maximum_bytes) {
    throw SqliteReadOnlyPreflightError(
        std::string(label) + " exceeds the preflight copy ceiling");
  }

  FileDescriptor destination(open(
      destination_path.c_str(),
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
      S_IRUSR | S_IWUSR));
  if (destination.get() < 0) {
    throw SqliteReadOnlyPreflightError(
        "cannot create private " + std::string(label) + " copy: " +
        std::string(std::strerror(errno)));
  }

  const auto expected_bytes =
      static_cast<std::uintmax_t>(before.size);
  std::uintmax_t copied_bytes = 0U;
  std::array<std::byte, 64U * 1024U> buffer{};
  while (copied_bytes < expected_bytes) {
    const auto remaining = expected_bytes - copied_bytes;
    const auto requested =
        std::min<std::uintmax_t>(remaining, buffer.size());
    const auto count = read(
        source.get(), buffer.data(), static_cast<std::size_t>(requested));
    if (count == 0) {
      throw SqliteReadOnlyPreflightError(
          std::string(label) + " changed size during preflight copy");
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw SqliteReadOnlyPreflightError(
          "cannot read " + std::string(label) + ": " +
          std::string(std::strerror(errno)));
    }
    const auto count_unsigned = static_cast<std::uintmax_t>(count);
    if (count_unsigned > maximum_bytes - copied_bytes) {
      throw SqliteReadOnlyPreflightError(
          std::string(label) + " grew beyond the preflight copy ceiling");
    }
    std::size_t offset = 0U;
    while (offset < static_cast<std::size_t>(count)) {
      const auto written = write(
          destination.get(),
          buffer.data() + offset,
          static_cast<std::size_t>(count) - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw SqliteReadOnlyPreflightError(
            "cannot write private " + std::string(label) + " copy: " +
            std::string(std::strerror(errno)));
      }
      if (written == 0) {
        throw SqliteReadOnlyPreflightError(
            "private " + std::string(label) + " copy made no progress");
      }
      offset += static_cast<std::size_t>(written);
    }
    copied_bytes += count_unsigned;
  }
  const auto after = StatusOf(source.get(), label);
  if (!SameStatus(before, after)) {
    throw SqliteReadOnlyPreflightError(
        std::string(label) + " changed during preflight copy");
  }
}

bool ExistingRegularFile(
    const std::filesystem::path& path,
    const std::string_view label) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::make_error_code(std::errc::no_such_file_or_directory) ||
      (!error &&
       status.type() == std::filesystem::file_type::not_found)) {
    return false;
  }
  if (error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    throw SqliteReadOnlyPreflightError(
        std::string(label) + " is not a non-symlink regular file");
  }
  return true;
}

std::uintmax_t BoundedFileSize(
    const std::filesystem::path& path,
    const std::uintmax_t maximum_bytes,
    const std::string_view label) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    throw SqliteReadOnlyPreflightError(
        "cannot inspect " + std::string(label) + " size");
  }
  if (size > maximum_bytes) {
    throw SqliteReadOnlyPreflightError(
        std::string(label) + " exceeds the preflight ceiling");
  }
  return size;
}

std::string UriEncodePath(const std::filesystem::path& path) {
  const auto absolute = std::filesystem::absolute(path).string();
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(absolute.size() + 32U);
  for (const auto character : absolute) {
    const auto byte = static_cast<unsigned char>(character);
    const bool unescaped =
        (byte >= 'a' && byte <= 'z') ||
        (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
        byte == '.' || byte == '~' || byte == '/';
    if (unescaped) {
      encoded.push_back(character);
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[(byte >> 4U) & 0x0FU]);
      encoded.push_back(kHex[byte & 0x0FU]);
    }
  }
  return "file:" + encoded + "?immutable=1";
}

void ValidateAgainstConnection(
    const std::string& filename,
    const int flags,
    const std::function<void(sqlite3*)>& validate) {
  sqlite3* database = nullptr;
  const auto result =
      sqlite3_open_v2(filename.c_str(), &database, flags, nullptr);
  if (result != SQLITE_OK) {
    const std::string detail =
        database == nullptr ? sqlite3_errstr(result) :
                              sqlite3_errmsg(database);
    if (database != nullptr) {
      sqlite3_close(database);
    }
    throw SqliteReadOnlyPreflightError(
        "cannot open read-only SQLite preflight: " + detail);
  }
  try {
    if (sqlite3_db_config(
            database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) !=
        SQLITE_OK) {
      throw SqliteReadOnlyPreflightError(
          "cannot disable trusted schema for SQLite preflight");
    }
    if (sqlite3_db_config(
            database, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) != SQLITE_OK) {
      throw SqliteReadOnlyPreflightError(
          "cannot enable defensive mode for SQLite preflight");
    }
    if (sqlite3_busy_timeout(database, 10000) != SQLITE_OK) {
      throw SqliteReadOnlyPreflightError(
          "cannot configure SQLite preflight busy timeout");
    }
    validate(database);
    if (sqlite3_close(database) != SQLITE_OK) {
      throw SqliteReadOnlyPreflightError(
          "cannot close read-only SQLite preflight");
    }
    database = nullptr;
  } catch (...) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    throw;
  }
}

}  // namespace

bool PreflightExistingSqliteReadOnly(
    const std::filesystem::path& database_path,
    const SqliteReadOnlyPreflightLimits limits,
    const std::function<void(sqlite3*)>& validate) {
  if (!validate) {
    throw std::invalid_argument(
        "SQLite preflight validation callback is required");
  }
  if (limits.maximum_main_database_bytes == 0U ||
      limits.maximum_wal_bytes == 0U) {
    throw std::invalid_argument(
        "SQLite preflight ceilings must be nonzero");
  }

  const auto wal_path =
      std::filesystem::path(database_path.string() + "-wal");
  const auto shm_path =
      std::filesystem::path(database_path.string() + "-shm");
  const auto rollback_path =
      std::filesystem::path(database_path.string() + "-journal");
  const bool database_exists =
      ExistingRegularFile(database_path, "SQLite database");
  const bool wal_exists = ExistingRegularFile(wal_path, "SQLite WAL");
  const bool shm_exists = ExistingRegularFile(shm_path, "SQLite SHM");
  const bool rollback_exists =
      ExistingRegularFile(rollback_path, "SQLite rollback journal");
  if (!database_exists) {
    if (wal_exists || shm_exists || rollback_exists) {
      throw SqliteReadOnlyPreflightError(
          "SQLite sidecar exists without its main database");
    }
    return false;
  }
  static_cast<void>(BoundedFileSize(
      database_path,
      limits.maximum_main_database_bytes,
      "SQLite database"));
  if (rollback_exists) {
    throw SqliteReadOnlyPreflightError(
        "SQLite rollback recovery residue cannot be accepted by the "
        "WAL-only durable-store preflight");
  }
  if (shm_exists && !wal_exists) {
    throw SqliteReadOnlyPreflightError(
        "SQLite SHM exists without its WAL");
  }

  if (wal_exists && !shm_exists) {
    TemporaryDirectory temporary;
    const auto copied_database = temporary.path() / "preflight.sqlite3";
    const auto copied_wal =
        std::filesystem::path(copied_database.string() + "-wal");
    CopyStableRegularFile(
        database_path,
        copied_database,
        limits.maximum_main_database_bytes,
        "SQLite database");
    CopyStableRegularFile(
        wal_path,
        copied_wal,
        limits.maximum_wal_bytes,
        "SQLite WAL");
    if (ExistingRegularFile(shm_path, "SQLite SHM")) {
      throw SqliteReadOnlyPreflightError(
          "SQLite SHM appeared during crash-residue preflight");
    }
    ValidateAgainstConnection(
        copied_database.string(),
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX |
            SQLITE_OPEN_NOFOLLOW,
        validate);
    return true;
  }

  if (wal_exists && shm_exists) {
    static_cast<void>(BoundedFileSize(
        wal_path, limits.maximum_wal_bytes, "SQLite WAL"));
    static_cast<void>(BoundedFileSize(
        shm_path, limits.maximum_wal_bytes, "SQLite SHM"));
    ValidateAgainstConnection(
        database_path.string(),
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX |
            SQLITE_OPEN_NOFOLLOW,
        validate);
    return true;
  }

  ValidateAgainstConnection(
      UriEncodePath(database_path),
      SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_FULLMUTEX |
          SQLITE_OPEN_NOFOLLOW,
      validate);
  return true;
}

}  // namespace manyforge::persistence
