#include "manyforge_deployment/build_manifest.hpp"

#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "build_manifest_internal.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "posix_durability_internal.hpp"

namespace manyforge::deployment {
namespace {

namespace posix = detail::posix_durability;

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

[[nodiscard]] std::exception_ptr make_manifest_error(
    std::string_view message) {
  return std::make_exception_ptr(BuildManifestError(std::string(message)));
}

const posix::ErrorPolicy kPosixErrors{make_manifest_error};
constexpr posix::TrustedRootMessages kTrustedRootMessages{
    "semantic-manifest trusted root must not be empty",
    "cannot resolve semantic-manifest trusted root",
    "semantic-manifest trusted root must be an existing plain directory",
};
constexpr posix::DirectoryMessages kDirectoryMessages{
    "cannot inspect semantic-manifest repository directory",
    "semantic-manifest repository path is not a plain directory",
    "cannot create semantic-manifest repository directory",
    "concurrently created semantic-manifest repository path is not a plain "
    "directory",
    "created semantic-manifest repository directory is unsafe",
};
constexpr posix::DirectorySyncMessages kDirectorySyncMessages{
    "cannot open semantic-manifest directory for fsync",
    "cannot fsync semantic-manifest directory",
};
constexpr posix::LockFileMessages kLockFileMessages{
    "cannot open semantic-manifest repository lock",
    "cannot stat semantic-manifest repository lock",
    "semantic-manifest repository lock is not a plain file",
    "cannot lock semantic-manifest repository",
    "filesystem semantic-manifest repository instances must be reopened "
    "after fork",
};
constexpr posix::ReadMessages kReadMessages{
    "cannot open semantic manifest",
    "cannot stat semantic manifest",
    "semantic manifest file violates its immutable byte bound",
    "semantic manifest file changed during exact read",
    "semantic manifest file grew during exact read",
};
constexpr posix::PublicationMessages kPublicationMessages{
    "cannot create semantic-manifest temporary file",
    "cannot allocate a unique semantic-manifest temporary file",
    "cannot write semantic-manifest temporary file",
    "cannot fsync semantic-manifest temporary file",
    "cannot close semantic-manifest temporary file",
    "cannot publish semantic manifest atomically",
    "cannot remove idempotent semantic-manifest temporary file",
    "cannot remove semantic-manifest temporary link",
};

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

}  // namespace

struct FilesystemBuildManifestRepository::Storage final {
  Storage(std::filesystem::path root_in,
          std::size_t max_manifest_count_in,
          std::uint64_t max_total_bytes_in)
      : root(std::move(root_in)),
        max_manifest_count(max_manifest_count_in),
        max_total_bytes(max_total_bytes_in) {}

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
        const auto bytes = posix::read_exact_regular_file(
            iterator->path(), max_total_bytes, false, kPosixErrors,
            kReadMessages);
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
      posix::sync_directory(
          kind_directory(root, kind), kDirectorySyncMessages);
    }
    manifest_count = refreshed_count;
    retained_bytes = refreshed_bytes;
  }

  void require_owner_process() const {
    lock->require_owner_process();
  }

  std::filesystem::path root;
  std::size_t max_manifest_count;
  std::uint64_t max_total_bytes;
  std::unique_ptr<posix::ProcessAffineLockFile> lock;
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
      posix::normalize_existing_plain_root(
          trusted_root, kPosixErrors, kTrustedRootMessages);
  const std::filesystem::path repository_root =
      absolute_root / "build-manifests";
  posix::ensure_owned_plain_directory(
      repository_root, kPosixErrors, kDirectoryMessages,
      kDirectorySyncMessages);
  for (const BuildManifestKind kind : kManifestKinds) {
    posix::ensure_owned_plain_directory(
        kind_directory(repository_root, kind), kPosixErrors,
        kDirectoryMessages, kDirectorySyncMessages);
  }
  storage_ = std::make_unique<Storage>(
      repository_root, max_manifest_count, max_total_bytes);
  const std::filesystem::path lock_path =
      storage_->root / ".repository.lock";
  storage_->lock = std::make_unique<posix::ProcessAffineLockFile>(
      lock_path, kPosixErrors, kLockFileMessages, kDirectorySyncMessages);
  std::lock_guard process_lock(storage_->mutex);
  const auto file_lock = storage_->lock->acquire(LOCK_EX);
  storage_->refresh_index_locked(true);
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
  const auto file_lock = storage_->lock->acquire(LOCK_EX);
  storage_->refresh_index_locked(true);
  const std::filesystem::path directory =
      kind_directory(storage_->root, manifest->key().manifest_kind);
  const std::filesystem::path final_path =
      directory / manifest_filename(manifest->key());
  if (const auto existing =
          posix::read_exact_regular_file(
              final_path, storage_->max_total_bytes, true, kPosixErrors,
              kReadMessages);
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

  const bool inserted = posix::publish_immutable_file_no_replace(
      final_path, manifest->canonical_bytes(), storage_->max_total_bytes,
      kPosixErrors, kReadMessages, kDirectorySyncMessages,
      kPublicationMessages,
      [&manifest](std::string_view existing) {
        if (existing != manifest->canonical_bytes()) {
          throw std::runtime_error(
              "filesystem semantic-manifest identity collision");
        }
      });
  if (!inserted) {
    return false;
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
  const auto file_lock = storage_->lock->acquire(LOCK_SH);
  storage_->refresh_index_locked(false);
  const auto bytes =
      posix::read_exact_regular_file(
          path, storage_->max_total_bytes, true, kPosixErrors,
          kReadMessages);
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
  const auto file_lock = storage_->lock->acquire(LOCK_SH);
  storage_->refresh_index_locked(false);
  return storage_->manifest_count;
}

std::uint64_t
FilesystemBuildManifestRepository::retained_bytes() const {
  storage_->require_owner_process();
  std::lock_guard process_lock(storage_->mutex);
  const auto file_lock = storage_->lock->acquire(LOCK_SH);
  storage_->refresh_index_locked(false);
  return storage_->retained_bytes;
}

}  // namespace manyforge::deployment
