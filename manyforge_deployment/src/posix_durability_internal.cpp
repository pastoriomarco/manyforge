#include "posix_durability_internal.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace manyforge::deployment::detail::posix_durability {
namespace {

[[noreturn]] void fail_semantic(const ErrorPolicy& errors,
                                std::string_view message) {
  if (errors.make_semantic_error == nullptr) {
    throw std::logic_error("POSIX durability semantic-error factory is null");
  }
  std::rethrow_exception(errors.make_semantic_error(message));
}

[[noreturn]] void fail_errno(std::string_view operation) {
  throw std::runtime_error(std::string(operation) + ": " +
                           std::strerror(errno));
}

void write_all_and_sync(int descriptor, std::string_view bytes,
                        const PublicationMessages& messages) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const ssize_t written =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      fail_errno(messages.write_temporary);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    fail_errno(messages.sync_temporary);
  }
}

}  // namespace

std::filesystem::path normalize_existing_plain_root(
    const std::filesystem::path& root, const ErrorPolicy& errors,
    const TrustedRootMessages& messages) {
  if (root.empty()) {
    fail_semantic(errors, messages.empty);
  }
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(root, error);
  if (error) {
    throw std::filesystem::filesystem_error(
        std::string(messages.resolve), root, error);
  }
  absolute = absolute.lexically_normal();
  while (absolute != absolute.root_path() && absolute.filename().empty()) {
    absolute = absolute.parent_path();
  }
  const auto status = std::filesystem::symlink_status(absolute, error);
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    fail_semantic(errors, messages.not_existing_plain_directory);
  }
  return absolute;
}

void sync_directory(const std::filesystem::path& directory,
                    const DirectorySyncMessages& messages) {
  const int descriptor =
      ::open(directory.c_str(),
             O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (descriptor < 0) {
    fail_errno(messages.open);
  }
  const int result = ::fsync(descriptor);
  const int saved_errno = errno;
  (void)::close(descriptor);
  if (result != 0) {
    errno = saved_errno;
    fail_errno(messages.sync);
  }
}

void ensure_owned_plain_directory(
    const std::filesystem::path& directory, const ErrorPolicy& errors,
    const DirectoryMessages& messages,
    const DirectorySyncMessages& parent_sync_messages) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(directory, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::filesystem::filesystem_error(
        std::string(messages.inspect), directory, error);
  }
  if (std::filesystem::exists(status)) {
    if (!std::filesystem::is_directory(status) ||
        std::filesystem::is_symlink(status)) {
      fail_semantic(errors, messages.existing_not_plain);
    }
  } else {
    const bool created = std::filesystem::create_directory(directory, error);
    if (error) {
      throw std::filesystem::filesystem_error(
          std::string(messages.create), directory, error);
    }
    if (!created) {
      const auto raced_status =
          std::filesystem::symlink_status(directory, error);
      if (error || !std::filesystem::is_directory(raced_status) ||
          std::filesystem::is_symlink(raced_status)) {
        fail_semantic(errors, messages.concurrently_created_not_plain);
      }
    } else {
      const auto created_status =
          std::filesystem::symlink_status(directory, error);
      if (error || !std::filesystem::is_directory(created_status) ||
          std::filesystem::is_symlink(created_status)) {
        fail_semantic(errors, messages.created_not_plain);
      }
    }
  }
  sync_directory(directory.parent_path(), parent_sync_messages);
}

ScopedAdvisoryLock::ScopedAdvisoryLock(
    int descriptor, int operation, std::string_view failure_message)
    : descriptor_(descriptor) {
  if (::flock(descriptor_, operation) != 0) {
    fail_errno(failure_message);
  }
}

ScopedAdvisoryLock::~ScopedAdvisoryLock() {
  (void)::flock(descriptor_, LOCK_UN);
}

ProcessAffineLockFile::ProcessAffineLockFile(
    const std::filesystem::path& path, const ErrorPolicy& errors,
    const LockFileMessages& messages,
    const DirectorySyncMessages& parent_sync_messages)
    : errors_(errors),
      lock_failure_message_(messages.lock),
      inherited_process_message_(messages.inherited_process),
      owner_pid_(::getpid()) {
  descriptor_ =
      ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor_ < 0) {
    fail_errno(messages.open);
  }
  try {
    struct stat status {};
    if (::fstat(descriptor_, &status) != 0) {
      fail_errno(messages.stat);
    }
    if (!S_ISREG(status.st_mode)) {
      fail_semantic(errors_, messages.not_plain);
    }
    sync_directory(path.parent_path(), parent_sync_messages);
  } catch (...) {
    const int saved_errno = errno;
    (void)::close(descriptor_);
    descriptor_ = -1;
    errno = saved_errno;
    throw;
  }
}

ProcessAffineLockFile::~ProcessAffineLockFile() {
  if (descriptor_ >= 0) {
    (void)::close(descriptor_);
  }
}

void ProcessAffineLockFile::require_owner_process() const {
  if (::getpid() != owner_pid_) {
    fail_semantic(errors_, inherited_process_message_);
  }
}

ScopedAdvisoryLock ProcessAffineLockFile::acquire(int operation) const {
  require_owner_process();
  return ScopedAdvisoryLock(
      descriptor_, operation, lock_failure_message_);
}

std::optional<std::string> read_exact_regular_file(
    const std::filesystem::path& path, std::uint64_t maximum_bytes,
    bool missing_is_empty, const ErrorPolicy& errors,
    const ReadMessages& messages) {
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (missing_is_empty && errno == ENOENT) {
      return std::nullopt;
    }
    fail_errno(messages.open);
  }
  struct Close final {
    int descriptor;
    ~Close() {
      if (descriptor >= 0) {
        (void)::close(descriptor);
      }
    }
  } close{descriptor};

  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    fail_errno(messages.stat);
  }
  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > maximum_bytes ||
      static_cast<std::uint64_t>(status.st_size) >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    fail_semantic(errors, messages.immutable_bound);
  }

  std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const ssize_t read_count =
        ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (read_count < 0 && errno == EINTR) {
      continue;
    }
    if (read_count <= 0) {
      fail_semantic(errors, messages.changed);
    }
    offset += static_cast<std::size_t>(read_count);
  }
  char extra = '\0';
  ssize_t extra_count = 0;
  do {
    extra_count = ::read(descriptor, &extra, 1U);
  } while (extra_count < 0 && errno == EINTR);
  if (extra_count != 0) {
    fail_semantic(errors, messages.grew);
  }
  return bytes;
}

bool publish_immutable_file_no_replace(
    const std::filesystem::path& final_path, std::string_view bytes,
    std::uint64_t maximum_existing_bytes, const ErrorPolicy& errors,
    const ReadMessages& read_messages,
    const DirectorySyncMessages& directory_sync_messages,
    const PublicationMessages& publication_messages,
    const ExistingValueVerifier& verify_existing) {
  static std::atomic<std::uint64_t> sequence{0U};
  const std::filesystem::path directory = final_path.parent_path();
  std::filesystem::path temporary_path;
  int temporary_descriptor = -1;
  for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
    std::string temporary_name{"."};
    temporary_name.append(final_path.filename().string());
    temporary_name.append(".tmp.");
    temporary_name.append(
        std::to_string(static_cast<long long>(::getpid())));
    temporary_name.push_back('.');
    temporary_name.append(std::to_string(sequence.fetch_add(1U)));
    temporary_path = directory / temporary_name;
    temporary_descriptor =
        ::open(temporary_path.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
               0600);
    if (temporary_descriptor >= 0) {
      break;
    }
    if (errno != EEXIST) {
      fail_errno(publication_messages.create_temporary);
    }
  }
  if (temporary_descriptor < 0) {
    fail_semantic(errors, publication_messages.allocate_temporary);
  }

  try {
    write_all_and_sync(
        temporary_descriptor, bytes, publication_messages);
    if (::close(temporary_descriptor) != 0) {
      temporary_descriptor = -1;
      fail_errno(publication_messages.close_temporary);
    }
    temporary_descriptor = -1;
    if (::link(temporary_path.c_str(), final_path.c_str()) != 0) {
      if (errno != EEXIST) {
        fail_errno(publication_messages.publish);
      }
      const auto existing = read_exact_regular_file(
          final_path, maximum_existing_bytes, false, errors, read_messages);
      if (!existing.has_value()) {
        throw std::logic_error(
            "existing immutable publication disappeared");
      }
      verify_existing(*existing);
      if (::unlink(temporary_path.c_str()) != 0) {
        fail_errno(publication_messages.remove_idempotent_temporary);
      }
      sync_directory(directory, directory_sync_messages);
      return false;
    }
    if (::unlink(temporary_path.c_str()) != 0) {
      fail_errno(publication_messages.remove_temporary_link);
    }
    sync_directory(directory, directory_sync_messages);
  } catch (...) {
    const int saved_errno = errno;
    if (temporary_descriptor >= 0) {
      (void)::close(temporary_descriptor);
    }
    (void)::unlink(temporary_path.c_str());
    errno = saved_errno;
    throw;
  }
  return true;
}

}  // namespace manyforge::deployment::detail::posix_durability
