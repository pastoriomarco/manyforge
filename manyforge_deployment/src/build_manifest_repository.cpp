#include "manyforge_deployment/build_manifest.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "build_manifest_internal.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::array kManifestKinds{
    BuildManifestKind::kNodeEnvironment,
    BuildManifestKind::kCompilerContract,
    BuildManifestKind::kCompilerBuild,
};
constexpr std::size_t kMaximumStaleTemporaryManifestCount = 256U;

[[noreturn]] void fail(std::string_view message) {
  throw BuildManifestError(std::string(message));
}

[[noreturn]] void fail_errno(std::string_view operation) {
  throw std::runtime_error(
      std::string(operation) + ": " + std::strerror(errno));
}

[[nodiscard]] std::filesystem::path kind_directory(
    const std::filesystem::path& root, BuildManifestKind kind) {
  const std::string_view kind_id = to_string(kind);
  if (kind_id.empty()) {
    fail("filesystem manifest key has an invalid kind");
  }
  return root / std::string(kind_id);
}

[[nodiscard]] std::string manifest_filename(
    const BuildManifestKey& key) {
  const std::string& digest = key.content_digest.value();
  if (!is_sha256_digest(digest)) {
    fail("filesystem manifest key has an invalid digest");
  }
  return digest.substr(std::string_view("sha256:").size()) + ".json";
}

[[nodiscard]] bool is_lower_hex(std::string_view value) noexcept {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char byte) {
           return (byte >= '0' && byte <= '9') ||
                  (byte >= 'a' && byte <= 'f');
         });
}

[[nodiscard]] BuildManifestKey key_from_filename(
    BuildManifestKind kind, std::string_view filename) {
  constexpr std::size_t kHexLength = 64U;
  constexpr std::string_view kSuffix = ".json";
  if (filename.size() != kHexLength + kSuffix.size() ||
      !filename.ends_with(kSuffix) ||
      !is_lower_hex(filename.substr(0U, kHexLength))) {
    fail("filesystem manifest repository contains an unknown entry");
  }
  return {
      kind,
      core::ContentDigest(
          "sha256:" + std::string(filename.substr(0U, kHexLength))),
  };
}

[[nodiscard]] bool is_stale_temporary_filename(
    std::string_view filename) noexcept {
  constexpr std::size_t kHexLength = 64U;
  constexpr std::string_view kPrefix = ".";
  constexpr std::string_view kMiddle = ".json.tmp.";
  return filename.starts_with(kPrefix) &&
         filename.size() > kPrefix.size() + kHexLength +
                               kMiddle.size() &&
         is_lower_hex(
             filename.substr(kPrefix.size(), kHexLength)) &&
         filename.substr(kPrefix.size() + kHexLength)
             .starts_with(kMiddle);
}

[[nodiscard]] std::filesystem::path absolute_trusted_root(
    const std::filesystem::path& root) {
  if (root.empty()) {
    fail("semantic-manifest trusted root must not be empty");
  }
  std::error_code error;
  std::filesystem::path absolute =
      std::filesystem::absolute(root, error);
  if (error) {
    throw std::filesystem::filesystem_error(
        "cannot resolve semantic-manifest trusted root", root, error);
  }
  absolute = absolute.lexically_normal();
  while (absolute != absolute.root_path() &&
         absolute.filename().empty()) {
    absolute = absolute.parent_path();
  }
  const auto status =
      std::filesystem::symlink_status(absolute, error);
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    fail("semantic-manifest trusted root must be an existing plain "
         "directory");
  }
  return absolute;
}

[[nodiscard]] bool ensure_plain_directory(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error &&
      error != std::errc::no_such_file_or_directory) {
    throw std::filesystem::filesystem_error(
        "cannot inspect semantic-manifest repository directory", path,
        error);
  }
  if (std::filesystem::exists(status)) {
    if (!std::filesystem::is_directory(status) ||
        std::filesystem::is_symlink(status)) {
      fail("semantic-manifest repository path is not a plain directory");
    }
    return false;
  }
  const bool created = std::filesystem::create_directory(path, error);
  if (error) {
    throw std::filesystem::filesystem_error(
        "cannot create semantic-manifest repository directory", path,
        error);
  }
  if (!created) {
    const auto raced_status =
        std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_directory(raced_status) ||
        std::filesystem::is_symlink(raced_status)) {
      fail("concurrently created semantic-manifest repository path is "
           "not a plain directory");
    }
    return false;
  }
  const auto created_status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_directory(created_status) ||
      std::filesystem::is_symlink(created_status)) {
    fail("created semantic-manifest repository directory is unsafe");
  }
  return true;
}

class AdvisoryFileLock final {
 public:
  AdvisoryFileLock(int fd, int operation) : fd_(fd) {
    if (::flock(fd_, operation) != 0) {
      fail_errno("cannot lock semantic-manifest repository");
    }
  }
  ~AdvisoryFileLock() { (void)::flock(fd_, LOCK_UN); }

  AdvisoryFileLock(const AdvisoryFileLock&) = delete;
  AdvisoryFileLock& operator=(const AdvisoryFileLock&) = delete;

 private:
  int fd_;
};

[[nodiscard]] std::optional<std::string> read_plain_file(
    const std::filesystem::path& path, std::uint64_t maximum_bytes,
    bool missing_is_empty) {
  const int fd =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (missing_is_empty && errno == ENOENT) {
      return std::nullopt;
    }
    fail_errno("cannot open semantic manifest");
  }
  struct Close final {
    int fd;
    ~Close() {
      if (fd >= 0) {
        (void)::close(fd);
      }
    }
  } close{fd};

  struct stat status {};
  if (::fstat(fd, &status) != 0) {
    fail_errno("cannot stat semantic manifest");
  }
  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > maximum_bytes ||
      static_cast<std::uint64_t>(status.st_size) >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    fail("semantic manifest file violates its immutable byte bound");
  }
  std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const ssize_t read_count =
        ::read(fd, bytes.data() + offset, bytes.size() - offset);
    if (read_count < 0 && errno == EINTR) {
      continue;
    }
    if (read_count <= 0) {
      fail("semantic manifest file changed during exact read");
    }
    offset += static_cast<std::size_t>(read_count);
  }
  char extra = '\0';
  ssize_t extra_count;
  do {
    extra_count = ::read(fd, &extra, 1U);
  } while (extra_count < 0 && errno == EINTR);
  if (extra_count != 0) {
    fail("semantic manifest file grew during exact read");
  }
  return bytes;
}

void fsync_directory(const std::filesystem::path& directory) {
  const int fd =
      ::open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                                      O_NOFOLLOW);
  if (fd < 0) {
    fail_errno("cannot open semantic-manifest directory for fsync");
  }
  const int result = ::fsync(fd);
  const int saved_errno = errno;
  (void)::close(fd);
  if (result != 0) {
    errno = saved_errno;
    fail_errno("cannot fsync semantic-manifest directory");
  }
}

void write_all_and_fsync(int fd, std::string_view bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const ssize_t written =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      fail_errno("cannot write semantic-manifest temporary file");
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(fd) != 0) {
    fail_errno("cannot fsync semantic-manifest temporary file");
  }
}

}  // namespace

struct FilesystemBuildManifestRepository::Storage final {
  Storage(std::filesystem::path root_in,
          std::size_t max_manifest_count_in,
          std::uint64_t max_total_bytes_in)
      : root(std::move(root_in)),
        max_manifest_count(max_manifest_count_in),
        max_total_bytes(max_total_bytes_in),
        owner_pid(::getpid()) {}

  ~Storage() {
    if (lock_fd >= 0) {
      (void)::close(lock_fd);
    }
  }

  void refresh_index_locked(bool remove_stale_temporaries) {
    std::size_t refreshed_count = 0U;
    std::uint64_t refreshed_bytes = 0U;
    std::size_t stale_temporary_count = 0U;
    std::uint64_t stale_temporary_bytes = 0U;
    std::vector<std::filesystem::path> stale_temporaries;
    const std::size_t maximum_stale_temporary_count =
        max_manifest_count >
                kMaximumStaleTemporaryManifestCount / 4U
            ? kMaximumStaleTemporaryManifestCount
            : max_manifest_count * 4U;
    for (const BuildManifestKind kind : kManifestKinds) {
      const std::filesystem::path directory =
          kind_directory(root, kind);
      std::error_code iteration_error;
      for (std::filesystem::directory_iterator iterator(
               directory, std::filesystem::directory_options::none,
               iteration_error),
           end;
           iterator != end; iterator.increment(iteration_error)) {
        if (iteration_error) {
          throw std::filesystem::filesystem_error(
              "cannot enumerate semantic-manifest repository", directory,
              iteration_error);
        }
        const std::string filename =
            iterator->path().filename().string();
        if (is_stale_temporary_filename(filename)) {
          const auto status =
              iterator->symlink_status(iteration_error);
          if (iteration_error ||
              !std::filesystem::is_regular_file(status) ||
              std::filesystem::is_symlink(status)) {
            fail("semantic-manifest repository contains an unsafe "
                 "temporary entry");
          }
          const std::uintmax_t byte_size =
              iterator->file_size(iteration_error);
          if (iteration_error ||
              byte_size > max_total_bytes ||
              ++stale_temporary_count >
                  maximum_stale_temporary_count ||
              static_cast<std::uint64_t>(byte_size) >
                  max_total_bytes - stale_temporary_bytes) {
            fail("semantic-manifest repository temporary-file bound is "
                 "exhausted");
          }
          stale_temporary_bytes +=
              static_cast<std::uint64_t>(byte_size);
          stale_temporaries.push_back(iterator->path());
          continue;
        }
        const auto status =
            iterator->symlink_status(iteration_error);
        if (iteration_error ||
            !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status)) {
          fail("semantic-manifest repository contains an unsafe entry");
        }
        const BuildManifestKey key =
            key_from_filename(kind, filename);
        const std::uintmax_t byte_size =
            iterator->file_size(iteration_error);
        if (iteration_error ||
            refreshed_count >= max_manifest_count ||
            byte_size >
                static_cast<std::uintmax_t>(
                    max_total_bytes - refreshed_bytes)) {
          fail("persistent semantic-manifest repository exceeds bounds");
        }
        const auto bytes =
            read_plain_file(iterator->path(), max_total_bytes, false);
        if (!bytes.has_value()) {
          throw std::logic_error(
              "enumerated semantic manifest disappeared");
        }
        if (refreshed_count >= max_manifest_count ||
            static_cast<std::uint64_t>(bytes->size()) >
                max_total_bytes - refreshed_bytes) {
          fail("persistent semantic-manifest repository exceeds bounds");
        }
        const auto reloaded =
            detail::reload_self_verified_build_manifest(kind, *bytes);
        if (reloaded->key() != key) {
          fail("semantic-manifest filename does not match its content");
        }
        ++refreshed_count;
        refreshed_bytes +=
            static_cast<std::uint64_t>(bytes->size());
      }
      if (iteration_error) {
        throw std::filesystem::filesystem_error(
            "cannot enumerate semantic-manifest repository", directory,
            iteration_error);
      }
    }
    if (remove_stale_temporaries) {
      for (const auto& temporary : stale_temporaries) {
        if (::unlink(temporary.c_str()) != 0) {
          fail_errno(
              "cannot remove stale semantic-manifest temporary file");
        }
      }
    }
    // A successful scan adopts every visible immutable final entry. This is
    // deliberately unconditional: it closes the crash window in which a
    // prior writer linked and unlinked its temporary name but died before
    // fsyncing the kind directory.
    for (const BuildManifestKind kind : kManifestKinds) {
      fsync_directory(kind_directory(root, kind));
    }
    manifest_count = refreshed_count;
    retained_bytes = refreshed_bytes;
  }

  void require_owner_process() const {
    if (::getpid() != owner_pid) {
      fail("filesystem semantic-manifest repository instances must be "
           "reopened after fork");
    }
  }

  std::filesystem::path root;
  std::size_t max_manifest_count;
  std::uint64_t max_total_bytes;
  pid_t owner_pid;
  int lock_fd{-1};
  std::size_t manifest_count{0U};
  std::uint64_t retained_bytes{0U};
  mutable std::mutex mutex;
};

FilesystemBuildManifestRepository::FilesystemBuildManifestRepository(
    std::filesystem::path trusted_root, std::size_t max_manifest_count,
    std::uint64_t max_total_bytes) {
  if (trusted_root.empty() || max_manifest_count == 0U ||
      max_total_bytes == 0U) {
    fail("filesystem semantic-manifest repository configuration is invalid");
  }
  const std::filesystem::path absolute_root =
      absolute_trusted_root(trusted_root);
  const std::filesystem::path repository_root =
      absolute_root / "build-manifests";
  (void)ensure_plain_directory(repository_root);
  fsync_directory(absolute_root);
  for (const BuildManifestKind kind : kManifestKinds) {
    (void)ensure_plain_directory(
        kind_directory(repository_root, kind));
    fsync_directory(repository_root);
  }
  storage_ = std::make_unique<Storage>(
      repository_root, max_manifest_count, max_total_bytes);
  const std::filesystem::path lock_path =
      storage_->root / ".repository.lock";
  storage_->lock_fd = ::open(
      lock_path.c_str(),
      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (storage_->lock_fd < 0) {
    fail_errno("cannot open semantic-manifest repository lock");
  }
  try {
    struct stat lock_status {};
    if (::fstat(storage_->lock_fd, &lock_status) != 0) {
      fail_errno("cannot stat semantic-manifest repository lock");
    }
    if (!S_ISREG(lock_status.st_mode)) {
      fail("semantic-manifest repository lock is not a plain file");
    }
    fsync_directory(storage_->root);

    std::lock_guard process_lock(storage_->mutex);
    const AdvisoryFileLock file_lock(storage_->lock_fd, LOCK_EX);
    storage_->refresh_index_locked(true);
  } catch (...) {
    const int saved_errno = errno;
    (void)::close(storage_->lock_fd);
    storage_->lock_fd = -1;
    errno = saved_errno;
    throw;
  }
}

FilesystemBuildManifestRepository::~FilesystemBuildManifestRepository() =
    default;

bool FilesystemBuildManifestRepository::put_if_absent(
    std::shared_ptr<const BuildQualifiedManifest> manifest) {
  storage_->require_owner_process();
  if (manifest == nullptr) {
    fail("filesystem semantic-manifest repository rejects null");
  }
  const std::uint64_t byte_size =
      static_cast<std::uint64_t>(manifest->canonical_bytes().size());
  if (byte_size > storage_->max_total_bytes) {
    fail("filesystem semantic-manifest value exceeds its repository byte "
         "bound");
  }
  const auto reloaded =
      detail::reload_self_verified_build_manifest(
          manifest->key().manifest_kind,
          manifest->canonical_bytes());
  if (reloaded->key() != manifest->key()) {
    fail("filesystem semantic-manifest key/content mismatch");
  }

  std::lock_guard process_lock(storage_->mutex);
  const AdvisoryFileLock file_lock(storage_->lock_fd, LOCK_EX);
  storage_->refresh_index_locked(true);
  const std::filesystem::path directory =
      kind_directory(storage_->root, manifest->key().manifest_kind);
  const std::filesystem::path final_path =
      directory / manifest_filename(manifest->key());
  if (const auto existing =
          read_plain_file(final_path, storage_->max_total_bytes, true);
      existing.has_value()) {
    const auto existing_manifest =
        detail::reload_self_verified_build_manifest(
            manifest->key().manifest_kind, *existing);
    if (existing_manifest->key() != manifest->key() ||
        existing_manifest->canonical_bytes() !=
            manifest->canonical_bytes()) {
      throw std::runtime_error(
          "filesystem semantic-manifest identity collision");
    }
    return false;
  }

  if (storage_->manifest_count >= storage_->max_manifest_count ||
      byte_size > storage_->max_total_bytes - storage_->retained_bytes) {
    fail("filesystem semantic-manifest repository bound is exhausted");
  }

  static std::atomic<std::uint64_t> sequence{0U};
  std::filesystem::path temporary_path;
  int temporary_fd = -1;
  for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
    std::string temporary_name{"."};
    temporary_name.append(manifest_filename(manifest->key()));
    temporary_name.append(".tmp.");
    temporary_name.append(
        std::to_string(static_cast<long long>(::getpid())));
    temporary_name.push_back('.');
    temporary_name.append(
        std::to_string(sequence.fetch_add(1U)));
    temporary_path = directory / temporary_name;
    temporary_fd = ::open(
        temporary_path.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (temporary_fd >= 0) {
      break;
    }
    if (errno != EEXIST) {
      fail_errno("cannot create semantic-manifest temporary file");
    }
  }
  if (temporary_fd < 0) {
    fail("cannot allocate a unique semantic-manifest temporary file");
  }

  try {
    write_all_and_fsync(temporary_fd, manifest->canonical_bytes());
    if (::close(temporary_fd) != 0) {
      temporary_fd = -1;
      fail_errno("cannot close semantic-manifest temporary file");
    }
    temporary_fd = -1;
    if (::link(temporary_path.c_str(), final_path.c_str()) != 0) {
      if (errno != EEXIST) {
        fail_errno("cannot publish semantic manifest atomically");
      }
      const auto existing = read_plain_file(
          final_path, storage_->max_total_bytes, false);
      if (!existing.has_value() ||
          *existing != manifest->canonical_bytes()) {
        throw std::runtime_error(
            "filesystem semantic-manifest identity collision");
      }
      if (::unlink(temporary_path.c_str()) != 0) {
        fail_errno(
            "cannot remove idempotent semantic-manifest temporary file");
      }
      fsync_directory(directory);
      return false;
    }
    if (::unlink(temporary_path.c_str()) != 0) {
      fail_errno("cannot remove semantic-manifest temporary link");
    }
    fsync_directory(directory);
  } catch (...) {
    const int saved_errno = errno;
    if (temporary_fd >= 0) {
      (void)::close(temporary_fd);
    }
    (void)::unlink(temporary_path.c_str());
    errno = saved_errno;
    throw;
  }
  ++storage_->manifest_count;
  storage_->retained_bytes += byte_size;
  return true;
}

std::shared_ptr<const BuildQualifiedManifest>
FilesystemBuildManifestRepository::get_verified(
    const BuildManifestKey& key) const {
  storage_->require_owner_process();
  const std::filesystem::path path =
      kind_directory(storage_->root, key.manifest_kind) /
      manifest_filename(key);
  std::lock_guard process_lock(storage_->mutex);
  const AdvisoryFileLock file_lock(storage_->lock_fd, LOCK_SH);
  storage_->refresh_index_locked(false);
  const auto bytes =
      read_plain_file(path, storage_->max_total_bytes, true);
  if (!bytes.has_value()) {
    return nullptr;
  }
  const auto manifest =
      detail::reload_self_verified_build_manifest(
          key.manifest_kind, *bytes);
  if (manifest->key() != key) {
    throw std::runtime_error(
        "filesystem semantic-manifest key/content mismatch");
  }
  return manifest;
}

std::size_t FilesystemBuildManifestRepository::size() const {
  storage_->require_owner_process();
  std::lock_guard process_lock(storage_->mutex);
  const AdvisoryFileLock file_lock(storage_->lock_fd, LOCK_SH);
  storage_->refresh_index_locked(false);
  return storage_->manifest_count;
}

std::uint64_t
FilesystemBuildManifestRepository::retained_bytes() const {
  storage_->require_owner_process();
  std::lock_guard process_lock(storage_->mutex);
  const AdvisoryFileLock file_lock(storage_->lock_fd, LOCK_SH);
  storage_->refresh_index_locked(false);
  return storage_->retained_bytes;
}

}  // namespace manyforge::deployment
