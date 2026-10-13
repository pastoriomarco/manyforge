#pragma once

#include <sqlite3.h>

#include <stdexcept>

namespace manyforge::core::test {

// SQLite's actual commit hook rolls back COMMIT. The optional real authorizer
// makes the ensuing durable readback unavailable, with no product injector.
struct CloseRetirementCommitFailure final {
  bool armed{};
  bool fail_readback{};
  bool deny_reads{};
  int commit_failures{};
};
inline thread_local CloseRetirementCommitFailure* close_retirement_commit_failure{};
inline int InstallCloseRetirementCommitFailure(sqlite3* database, char**,
                                               const sqlite3_api_routines*) {
  auto* control = close_retirement_commit_failure;
  if (!control) return SQLITE_OK;
  sqlite3_commit_hook(database, [](void* value) {
    auto& failure = *static_cast<CloseRetirementCommitFailure*>(value);
    if (!failure.armed) return 0;
    failure.armed = false;
    ++failure.commit_failures;
    failure.deny_reads = failure.fail_readback;
    return 1;
  }, control);
  return sqlite3_set_authorizer(database, [](void* value, int action,
      const char*, const char*, const char*, const char*) {
    const auto& failure = *static_cast<CloseRetirementCommitFailure*>(value);
    return failure.deny_reads && (action == SQLITE_SELECT || action == SQLITE_READ)
        ? SQLITE_DENY : SQLITE_OK;
  }, control);
}

class ScopedCloseRetirementCommitFailure final {
 public:
  explicit ScopedCloseRetirementCommitFailure(CloseRetirementCommitFailure& value) {
    close_retirement_commit_failure = &value;
    if (sqlite3_auto_extension(reinterpret_cast<void (*)()>(InstallCloseRetirementCommitFailure)) != SQLITE_OK)
      throw std::runtime_error("cannot install scoped SQLite Close commit failure");
  }
  ~ScopedCloseRetirementCommitFailure() {
    sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(InstallCloseRetirementCommitFailure));
    close_retirement_commit_failure = nullptr;
  }
};

}  // namespace manyforge::core::test
