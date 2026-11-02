#include <manyforge_persistence/sqlite_read_only_preflight.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::persistence {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() /
         "manyforge-persistence-test-XXXXXX")
            .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto* created = mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

void Exec(const std::filesystem::path& path, const std::string& sql) {
  sqlite3* database = nullptr;
  if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    throw std::runtime_error("cannot open SQLite test database");
  }
  char* error = nullptr;
  const auto result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string detail =
        error == nullptr ? sqlite3_errmsg(database) : error;
    sqlite3_free(error);
    sqlite3_close(database);
    throw std::runtime_error(detail);
  }
  if (sqlite3_close(database) != SQLITE_OK) {
    throw std::runtime_error("cannot close SQLite test database");
  }
}

[[nodiscard]] std::string ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read test file");
  }
  return {
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
}

[[nodiscard]] int ReadMarker(sqlite3* database) {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(
          database, "SELECT value FROM marker", -1, &statement,
          nullptr) != SQLITE_OK) {
    throw std::runtime_error("cannot prepare marker query");
  }
  const auto result = sqlite3_step(statement);
  if (result != SQLITE_ROW) {
    sqlite3_finalize(statement);
    throw std::runtime_error("marker row is missing");
  }
  const int value = sqlite3_column_int(statement, 0);
  if (sqlite3_step(statement) != SQLITE_DONE ||
      sqlite3_finalize(statement) != SQLITE_OK) {
    throw std::runtime_error("marker query is not a singleton");
  }
  return value;
}

constexpr SqliteReadOnlyPreflightLimits kLimits{
    8U * 1024U * 1024U,
    8U * 1024U * 1024U,
};

TEST(SqliteReadOnlyPreflight, MissingPathHasNoEffectAndInvalidUseRejects) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "missing.sqlite3";
  bool invoked = false;
  EXPECT_FALSE(PreflightExistingSqliteReadOnly(
      path, kLimits, [&invoked](sqlite3*) { invoked = true; }));
  EXPECT_FALSE(invoked);
  EXPECT_FALSE(std::filesystem::exists(path));

  EXPECT_THROW(
      static_cast<void>(PreflightExistingSqliteReadOnly(
          path, {0U, 1U}, [](sqlite3*) {})),
      std::invalid_argument);
  EXPECT_THROW(
      static_cast<void>(PreflightExistingSqliteReadOnly(
          path, kLimits, {})),
      std::invalid_argument);
}

TEST(SqliteReadOnlyPreflight, ReadsCleanDatabaseWithoutChangingIt) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "clean.sqlite3";
  Exec(path, "CREATE TABLE marker(value INTEGER NOT NULL);"
             "INSERT INTO marker VALUES(42);");
  const auto before = ReadBytes(path);

  int marker = 0;
  EXPECT_TRUE(PreflightExistingSqliteReadOnly(
      path, kLimits,
      [&marker](sqlite3* database) { marker = ReadMarker(database); }));
  EXPECT_EQ(marker, 42);
  EXPECT_EQ(ReadBytes(path), before);
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-wal"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-shm"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-journal"));
}

TEST(SqliteReadOnlyPreflight, RejectsUnsafePathsSidecarsAndBounds) {
  TemporaryDirectory directory;
  const auto real = directory.path() / "real.sqlite3";
  const auto alias = directory.path() / "alias.sqlite3";
  Exec(real, "CREATE TABLE marker(value INTEGER NOT NULL);"
             "INSERT INTO marker VALUES(1);");
  std::filesystem::create_symlink(real, alias);
  EXPECT_THROW(
      static_cast<void>(PreflightExistingSqliteReadOnly(
          alias, kLimits, [](sqlite3*) {})),
      SqliteReadOnlyPreflightError);

  const auto missing = directory.path() / "orphan.sqlite3";
  {
    std::ofstream orphan(missing.string() + "-wal");
    orphan << "orphan";
  }
  EXPECT_THROW(
      static_cast<void>(PreflightExistingSqliteReadOnly(
          missing, kLimits, [](sqlite3*) {})),
      SqliteReadOnlyPreflightError);

  {
    std::ofstream rollback(real.string() + "-journal");
    rollback << "residue";
  }
  EXPECT_THROW(
      static_cast<void>(PreflightExistingSqliteReadOnly(
          real, kLimits, [](sqlite3*) {})),
      SqliteReadOnlyPreflightError);
  std::filesystem::remove(real.string() + "-journal");

  EXPECT_THROW(
      static_cast<void>(PreflightExistingSqliteReadOnly(
          real, {1U, kLimits.maximum_wal_bytes}, [](sqlite3*) {})),
      SqliteReadOnlyPreflightError);
}

TEST(SqliteReadOnlyPreflight,
     ValidatesWalCrashResidueThroughCopyWithoutChangingSource) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "wal-residue.sqlite3";
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    sqlite3* database = nullptr;
    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
      _exit(1);
    }
    char* error = nullptr;
    const auto result = sqlite3_exec(
        database,
        "PRAGMA journal_mode=WAL;"
        "PRAGMA wal_autocheckpoint=0;"
        "CREATE TABLE marker(value INTEGER NOT NULL);"
        "INSERT INTO marker VALUES(73);",
        nullptr, nullptr, &error);
    sqlite3_free(error);
    static_cast<void>(database);
    _exit(result == SQLITE_OK ? 0 : 1);
  }

  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  const auto wal = std::filesystem::path(path.string() + "-wal");
  const auto shm = std::filesystem::path(path.string() + "-shm");
  ASSERT_TRUE(std::filesystem::exists(path));
  ASSERT_TRUE(std::filesystem::exists(wal));
  std::filesystem::remove(shm);
  const auto database_before = ReadBytes(path);
  const auto wal_before = ReadBytes(wal);

  int marker = 0;
  EXPECT_TRUE(PreflightExistingSqliteReadOnly(
      path, kLimits,
      [&marker](sqlite3* database) { marker = ReadMarker(database); }));
  EXPECT_EQ(marker, 73);
  EXPECT_EQ(ReadBytes(path), database_before);
  EXPECT_EQ(ReadBytes(wal), wal_before);
  EXPECT_FALSE(std::filesystem::exists(shm));
}

}  // namespace
}  // namespace manyforge::persistence
