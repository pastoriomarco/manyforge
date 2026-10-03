#pragma once

#include <sys/types.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace manyforge::deployment::detail::posix_durability {

// This module deliberately stops below repository semantics. Callers retain
// ownership of namespaces, filenames, bounds, enumeration, identity checks,
// and error taxonomies while sharing the POSIX durability mechanics.
struct ErrorPolicy final {
  using SemanticErrorFactory =
      std::exception_ptr (*)(std::string_view message);

  SemanticErrorFactory make_semantic_error;
};

struct TrustedRootMessages final {
  std::string_view empty;
  std::string_view resolve;
  std::string_view not_existing_plain_directory;
};

struct DirectoryMessages final {
  std::string_view inspect;
  std::string_view existing_not_plain;
  std::string_view create;
  std::string_view concurrently_created_not_plain;
  std::string_view created_not_plain;
};

struct ExistingDirectoryMessages final {
  std::string_view inspect;
  std::string_view not_existing_plain_directory;
};

struct DirectorySyncMessages final {
  std::string_view open;
  std::string_view sync;
};

struct LockFileMessages final {
  std::string_view open;
  std::string_view stat;
  std::string_view not_plain;
  std::string_view lock;
  std::string_view inherited_process;
};

struct ReadMessages final {
  std::string_view open;
  std::string_view stat;
  std::string_view immutable_bound;
  std::string_view changed;
  std::string_view grew;
};

struct PublicationMessages final {
  std::string_view create_temporary;
  std::string_view allocate_temporary;
  std::string_view write_temporary;
  std::string_view sync_temporary;
  std::string_view close_temporary;
  std::string_view publish;
  std::string_view remove_idempotent_temporary;
  std::string_view remove_temporary_link;
};

[[nodiscard]] std::filesystem::path normalize_existing_plain_root(
    const std::filesystem::path& root, const ErrorPolicy& errors,
    const TrustedRootMessages& messages);

void sync_directory(const std::filesystem::path& directory,
                    const DirectorySyncMessages& messages);

// Creates or adopts exactly one caller-owned directory, rejects symlinks and
// non-directories, then durably records the directory entry in its parent.
void ensure_owned_plain_directory(
    const std::filesystem::path& directory, const ErrorPolicy& errors,
    const DirectoryMessages& messages,
    const DirectorySyncMessages& parent_sync_messages);

// Read-only counterpart to ensure_owned_plain_directory. It never creates or
// syncs the path and rejects a symlink at the fixed component being checked.
void require_existing_plain_directory(
    const std::filesystem::path& directory, const ErrorPolicy& errors,
    const ExistingDirectoryMessages& messages);

class ScopedAdvisoryLock final {
 public:
  ~ScopedAdvisoryLock();

  ScopedAdvisoryLock(const ScopedAdvisoryLock&) = delete;
  ScopedAdvisoryLock& operator=(const ScopedAdvisoryLock&) = delete;
  ScopedAdvisoryLock(ScopedAdvisoryLock&&) = delete;
  ScopedAdvisoryLock& operator=(ScopedAdvisoryLock&&) = delete;

 private:
  friend class ProcessAffineLockFile;

  ScopedAdvisoryLock(int descriptor, int operation,
                     std::string_view failure_message);

  int descriptor_;
};

class ProcessAffineLockFile final {
 public:
  ProcessAffineLockFile(
      const std::filesystem::path& path, const ErrorPolicy& errors,
      const LockFileMessages& messages,
      const DirectorySyncMessages& parent_sync_messages);
  ~ProcessAffineLockFile();

  ProcessAffineLockFile(const ProcessAffineLockFile&) = delete;
  ProcessAffineLockFile& operator=(const ProcessAffineLockFile&) = delete;
  ProcessAffineLockFile(ProcessAffineLockFile&&) = delete;
  ProcessAffineLockFile& operator=(ProcessAffineLockFile&&) = delete;

  void require_owner_process() const;

  [[nodiscard]] ScopedAdvisoryLock acquire(int operation) const;

 private:
  ErrorPolicy errors_;
  std::string lock_failure_message_;
  std::string inherited_process_message_;
  pid_t owner_pid_;
  int descriptor_{-1};
};

[[nodiscard]] std::optional<std::string> read_exact_regular_file(
    const std::filesystem::path& path, std::uint64_t maximum_bytes,
    bool missing_is_empty, const ErrorPolicy& errors,
    const ReadMessages& messages);

/// Identity of one opened regular file version. Any replacement (new inode)
/// or in-place rewrite (mtime/ctime/size) yields a different value.
struct RegularFileIdentity final {
  std::uint64_t device{};
  std::uint64_t inode{};
  std::uint64_t size{};
  std::int64_t modified_seconds{};
  std::int64_t modified_nanoseconds{};
  std::int64_t changed_seconds{};
  std::int64_t changed_nanoseconds{};

  friend bool operator==(const RegularFileIdentity&,
                         const RegularFileIdentity&) = default;
};

struct IdentifiedFileBytes final {
  std::string bytes;
  /// Identity of the descriptor the bytes were read from, observed both
  /// before and after the exact read.
  RegularFileIdentity identity;
};

/// Current CLOCK_REALTIME, the clock of file change timestamps.
[[nodiscard]] std::int64_t realtime_now_ns();

/// True when the file version last changed comfortably before
/// `reference_realtime_ns`, by more than any filesystem timestamp granularity.
/// Only then must any write made after that reference change the identity;
/// a version changed within the same timestamp tick may not.
[[nodiscard]] bool identity_settled_before(const RegularFileIdentity& identity,
                                           std::int64_t reference_realtime_ns);

/// The exact-read contract of read_exact_regular_file(), also returning the
/// identity of the file version that was read.
[[nodiscard]] std::optional<IdentifiedFileBytes>
read_exact_regular_file_with_identity(const std::filesystem::path& path,
                                      std::uint64_t maximum_bytes,
                                      bool missing_is_empty,
                                      const ErrorPolicy& errors,
                                      const ReadMessages& messages);

/// Opens without following links and returns the current regular-file
/// identity under the same bound checks as the exact read, without reading.
[[nodiscard]] std::optional<RegularFileIdentity> regular_file_identity(
    const std::filesystem::path& path, std::uint64_t maximum_bytes,
    bool missing_is_empty, const ErrorPolicy& errors,
    const ReadMessages& messages);

using ExistingValueVerifier =
    std::function<void(std::string_view existing_bytes)>;

// Publishes a same-directory immutable file through
// temporary-write+fsync, hard-link-no-replace, temporary-unlink, and
// directory-fsync. The caller verifies an EEXIST winner according to its own
// identity rules.
[[nodiscard]] bool publish_immutable_file_no_replace(
    const std::filesystem::path& final_path, std::string_view bytes,
    std::uint64_t maximum_existing_bytes, const ErrorPolicy& errors,
    const ReadMessages& read_messages,
    const DirectorySyncMessages& directory_sync_messages,
    const PublicationMessages& publication_messages,
    const ExistingValueVerifier& verify_existing);

}  // namespace manyforge::deployment::detail::posix_durability
