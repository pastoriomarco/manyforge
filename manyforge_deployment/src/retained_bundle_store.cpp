#include "manyforge_deployment/retained_bundle.hpp"

#include <sys/file.h>
#include <unistd.h>

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

#include "manyforge_deployment/content_identity.hpp"
#include "catalog_artifact_store_internal.hpp"
#include "posix_durability_internal.hpp"
#include "program_binding_internal.hpp"
#include "retained_bundle_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

namespace posix = detail::posix_durability;

enum class DurableValueKind {
  kRawArtifact,
  kCatalogArtifact,
  kRetainedBundle,
  kProgramBinding,
};

struct DurableStoreLayout final {
  std::string_view directory_name;
  std::string_view data_directory_name;
  std::string_view file_suffix;
};

[[nodiscard]] constexpr DurableStoreLayout layout_for(
    DurableValueKind kind) noexcept {
  switch (kind) {
    case DurableValueKind::kRawArtifact:
      return {"raw-artifacts", "sha256", ".blob"};
    case DurableValueKind::kCatalogArtifact:
      return {detail::kCatalogArtifactNamespaceDirectory,
              detail::kCatalogArtifactDataDirectory,
              detail::kCatalogArtifactFileSuffix};
    case DurableValueKind::kRetainedBundle:
      return {"retained-bundles", "sha256", ".json"};
    case DurableValueKind::kProgramBinding:
      return {"program-bindings", "sha256", ".json"};
  }
  return {};
}

constexpr std::size_t kMaximumStaleTemporaryCount = 256U;
constexpr std::size_t kDigestHexLength = 64U;

[[noreturn]] void fail(std::string_view message) {
  throw RetainedBundleError(std::string(message));
}

[[noreturn]] void fail_errno(std::string_view operation) {
  throw std::runtime_error(std::string(operation) + ": " +
                           std::strerror(errno));
}

[[nodiscard]] std::exception_ptr make_retention_error(
    std::string_view message) {
  return std::make_exception_ptr(RetainedBundleError(std::string(message)));
}

const posix::ErrorPolicy kPosixErrors{make_retention_error};
constexpr posix::TrustedRootMessages kTrustedRootMessages{
    "durable retention trusted root must not be empty",
    "cannot resolve durable retention trusted root",
    "durable retention trusted root must be an existing plain directory",
};
constexpr posix::DirectoryMessages kDirectoryMessages{
    "cannot inspect durable retention directory",
    "durable retention path is not a plain directory",
    "cannot create durable retention directory",
    "concurrently created durable retention path is unsafe",
    "created durable retention directory is unsafe",
};
constexpr posix::DirectorySyncMessages kDirectorySyncMessages{
    "cannot open durable retention directory for fsync",
    "cannot fsync durable retention directory",
};
constexpr posix::LockFileMessages kLockFileMessages{
    "cannot open durable retention repository lock",
    "cannot stat durable retention repository lock",
    "durable retention repository lock is not a plain file",
    "cannot lock durable retention store",
    "durable retention store instances must be reopened after fork",
};
constexpr posix::ReadMessages kReadMessages{
    "cannot open durable retention entry",
    "cannot stat durable retention entry",
    "durable retention entry violates its immutable byte bound",
    "durable retention entry changed during exact read",
    "durable retention entry grew during exact read",
};
constexpr posix::PublicationMessages kPublicationMessages{
    "cannot create durable retention temporary file",
    "cannot allocate a unique durable retention temporary file",
    "cannot write durable retention temporary file",
    "cannot fsync durable retention temporary file",
    "cannot close durable retention temporary file",
    "cannot publish durable retention value atomically",
    "cannot remove idempotent durable retention temporary file",
    "cannot remove durable retention temporary link",
};

[[nodiscard]] bool is_stale_temporary_filename(
    std::string_view filename, std::string_view suffix) noexcept {
  if (!filename.starts_with('.') ||
      filename.size() <= 1U + kDigestHexLength + suffix.size()) {
    return false;
  }
  const std::string_view hex = filename.substr(1U, kDigestHexLength);
  const std::string_view remainder =
      filename.substr(1U + kDigestHexLength);
  return detail::is_retained_digest_hex(hex) &&
         remainder.starts_with(suffix) &&
         remainder.substr(suffix.size()).starts_with(".tmp.") &&
         remainder.size() > suffix.size() +
                                std::string_view(".tmp.").size();
}

[[nodiscard]] std::string hex_from_filename(
    std::string_view filename, std::string_view suffix) {
  if (filename.size() != kDigestHexLength + suffix.size() ||
      !filename.ends_with(suffix)) {
    fail("durable retention store contains an unknown entry");
  }
  const std::string_view hex = filename.substr(0U, kDigestHexLength);
  (void)detail::retained_digest_from_hex(hex);
  return std::string(hex);
}

[[nodiscard]] std::uint64_t verify_entry_bytes(
    DurableValueKind kind, std::string_view hex, std::string_view bytes) {
  const std::string expected_digest =
      detail::retained_digest_from_hex(hex);
  switch (kind) {
    case DurableValueKind::kRawArtifact:
      if (sha256_digest(bytes) != expected_digest) {
        fail("durable raw-artifact filename does not match its bytes");
      }
      return static_cast<std::uint64_t>(bytes.size());
    case DurableValueKind::kCatalogArtifact: {
      const auto decoded =
          detail::decode_catalog_artifact_envelope(bytes);
      const auto expected_hex =
          detail::catalog_artifact_storage_hex(
              decoded.content_addressed_store_key);
      if (!expected_hex.has_value() || *expected_hex != hex) {
        fail("durable catalog-artifact filename does not match its "
             "opaque key");
      }
      return static_cast<std::uint64_t>(decoded.payload.size());
    }
    case DurableValueKind::kRetainedBundle: {
      const auto bundle =
          detail::RetainedBundleReloadAuthority::reload(bytes);
      if (bundle->retained_bundle_id().value() != expected_digest) {
        fail("durable retained-bundle filename does not match its "
             "semantic identity");
      }
      return static_cast<std::uint64_t>(bytes.size());
    }
    case DurableValueKind::kProgramBinding: {
      const auto binding =
          detail::ProgramBindingConstructionAuthority::reload_intrinsic(
              bytes);
      if (binding->program_binding_id().value() != expected_digest) {
        fail("durable Program-Binding filename does not match its semantic "
             "identity");
      }
      return static_cast<std::uint64_t>(bytes.size());
    }
  }
  throw std::logic_error("unknown durable retention value kind");
}

class DurableFileCollection final {
 public:
  DurableFileCollection(DurableValueKind kind,
                        std::filesystem::path trusted_root,
                        std::size_t max_entry_count,
                        std::uint64_t max_total_bytes,
                        std::uint64_t max_single_entry_bytes,
                        std::uint64_t max_entry_overhead = 0U)
      : kind_(kind),
        layout_(layout_for(kind)),
        root_(posix::normalize_existing_plain_root(
            trusted_root, kPosixErrors, kTrustedRootMessages)),
        namespace_directory_(
            root_ / std::string(layout_.directory_name)),
        data_directory_(
            namespace_directory_ /
            std::string(layout_.data_directory_name)),
        max_entry_count_(max_entry_count),
        max_total_bytes_(max_total_bytes),
        max_single_entry_bytes_(max_single_entry_bytes),
        max_entry_overhead_(max_entry_overhead) {
    if (max_entry_count_ == 0U || max_total_bytes_ == 0U ||
        max_single_entry_bytes_ == 0U ||
        max_single_entry_bytes_ > max_total_bytes_) {
      fail("durable retention limits must be positive and internally "
           "consistent");
    }
    if (max_entry_overhead_ >
            std::numeric_limits<std::uint64_t>::max() -
                max_single_entry_bytes_ ||
        (max_entry_overhead_ != 0U &&
         max_entry_count_ >
             (std::numeric_limits<std::uint64_t>::max() -
              max_total_bytes_) /
                 max_entry_overhead_)) {
      fail("durable retention encoded-size bounds overflow");
    }
    max_single_stored_entry_bytes_ =
        max_single_entry_bytes_ + max_entry_overhead_;
    max_total_stored_bytes_ =
        max_total_bytes_ +
        static_cast<std::uint64_t>(max_entry_count_) *
            max_entry_overhead_;

    posix::ensure_owned_plain_directory(
        namespace_directory_, kPosixErrors, kDirectoryMessages,
        kDirectorySyncMessages);
    posix::ensure_owned_plain_directory(
        data_directory_, kPosixErrors, kDirectoryMessages,
        kDirectorySyncMessages);
    const std::filesystem::path lock_path =
        namespace_directory_ / ".repository.lock";
    lock_ = std::make_unique<posix::ProcessAffineLockFile>(
        lock_path, kPosixErrors, kLockFileMessages,
        kDirectorySyncMessages);

    std::lock_guard process_lock(mutex_);
    const auto file_lock = lock_->acquire(LOCK_EX);
    refresh_index_locked(true);
  }

  DurableFileCollection(const DurableFileCollection&) = delete;
  DurableFileCollection& operator=(const DurableFileCollection&) = delete;

  void require_owner_process() const {
    lock_->require_owner_process();
  }

  void require_insert_size(std::size_t byte_size) const {
    require_owner_process();
    if (byte_size > max_single_entry_bytes_) {
      fail("durable retention value exceeds its single-entry bound");
    }
  }

  [[nodiscard]] bool put_if_absent(std::string_view hex,
                                   std::string_view bytes) {
    require_owner_process();
    if (bytes.size() > max_single_stored_entry_bytes_) {
      fail("durable retention encoded value exceeds its single-entry "
           "bound");
    }
    (void)detail::retained_digest_from_hex(hex);
    const std::uint64_t logical_byte_size =
        verify_entry_bytes(kind_, hex, bytes);
    if (logical_byte_size > max_single_entry_bytes_) {
      fail("durable retention value exceeds its single-entry bound");
    }

    std::lock_guard process_lock(mutex_);
    const auto file_lock = lock_->acquire(LOCK_EX);
    refresh_index_locked(true);
    const std::filesystem::path final_path =
        data_directory_ / committed_filename(hex);
    if (const auto existing =
            posix::read_exact_regular_file(
                final_path, max_single_stored_entry_bytes_, true,
                kPosixErrors,
                kReadMessages);
        existing.has_value()) {
      (void)verify_entry_bytes(kind_, hex, *existing);
      if (*existing != bytes) {
        throw std::runtime_error(
            "durable retention identity collision");
      }
      return false;
    }

    const std::uint64_t stored_byte_size =
        static_cast<std::uint64_t>(bytes.size());
    if (entry_count_ >= max_entry_count_ ||
        logical_byte_size > max_total_bytes_ - retained_bytes_ ||
        stored_byte_size >
            max_total_stored_bytes_ - stored_bytes_) {
      fail("durable retention repository bound is exhausted");
    }

    const bool inserted = posix::publish_immutable_file_no_replace(
        final_path, bytes, max_single_stored_entry_bytes_, kPosixErrors,
        kReadMessages, kDirectorySyncMessages, kPublicationMessages,
        [this, hex, bytes](std::string_view existing) {
          if (existing != bytes) {
            throw std::runtime_error(
                "durable retention identity collision");
          }
          (void)verify_entry_bytes(kind_, hex, existing);
        });
    if (!inserted) {
      return false;
    }
    ++entry_count_;
    retained_bytes_ += logical_byte_size;
    stored_bytes_ += stored_byte_size;
    return true;
  }

  [[nodiscard]] std::optional<std::string> get_verified(
      std::string_view hex) {
    require_owner_process();
    (void)detail::retained_digest_from_hex(hex);
    std::lock_guard process_lock(mutex_);
    const auto file_lock = lock_->acquire(LOCK_SH);
    const auto bytes = posix::read_exact_regular_file(
        data_directory_ / committed_filename(hex),
        max_single_stored_entry_bytes_, true, kPosixErrors, kReadMessages);
    if (!bytes.has_value()) {
      return std::nullopt;
    }
    (void)verify_entry_bytes(kind_, hex, *bytes);
    // A successful point read adopts a visible immutable final left by a
    // writer that may have died after link + temporary unlink but before its
    // directory fsync.
    posix::sync_directory(data_directory_, kDirectorySyncMessages);
    return bytes;
  }

  [[nodiscard]] std::size_t size() {
    require_owner_process();
    std::lock_guard process_lock(mutex_);
    const auto file_lock = lock_->acquire(LOCK_SH);
    refresh_index_locked(false);
    return entry_count_;
  }

  [[nodiscard]] std::uint64_t retained_bytes() {
    require_owner_process();
    std::lock_guard process_lock(mutex_);
    const auto file_lock = lock_->acquire(LOCK_SH);
    refresh_index_locked(false);
    return retained_bytes_;
  }

  [[nodiscard]] const std::filesystem::path& trusted_root() const {
    require_owner_process();
    return root_;
  }

 private:
  [[nodiscard]] std::string committed_filename(
      std::string_view hex) const {
    return std::string(hex) + std::string(layout_.file_suffix);
  }

  void refresh_index_locked(bool remove_stale_temporaries) {
    std::size_t refreshed_count = 0U;
    std::uint64_t refreshed_bytes = 0U;
    std::uint64_t refreshed_stored_bytes = 0U;
    std::size_t stale_count = 0U;
    std::uint64_t stale_bytes = 0U;
    std::vector<std::filesystem::path> stale_temporaries;
    const std::size_t maximum_stale_count =
        max_entry_count_ >
                kMaximumStaleTemporaryCount / 4U
            ? kMaximumStaleTemporaryCount
            : max_entry_count_ * 4U;

    std::error_code iteration_error;
    for (std::filesystem::directory_iterator iterator(
             data_directory_, std::filesystem::directory_options::none,
             iteration_error),
         end;
         iterator != end; iterator.increment(iteration_error)) {
      if (iteration_error) {
        throw std::filesystem::filesystem_error(
            "cannot enumerate durable retention repository",
            data_directory_, iteration_error);
      }
      const std::string filename = iterator->path().filename().string();
      const auto status = iterator->symlink_status(iteration_error);
      if (iteration_error || !std::filesystem::is_regular_file(status) ||
          std::filesystem::is_symlink(status)) {
        fail("durable retention repository contains an unsafe entry");
      }

      if (is_stale_temporary_filename(filename, layout_.file_suffix)) {
        const std::uintmax_t byte_size =
            iterator->file_size(iteration_error);
        if (iteration_error ||
            byte_size > max_single_stored_entry_bytes_ ||
            ++stale_count > maximum_stale_count ||
            static_cast<std::uint64_t>(byte_size) >
                max_total_stored_bytes_ - stale_bytes) {
          fail("durable retention temporary-file bound is exhausted");
        }
        stale_bytes += static_cast<std::uint64_t>(byte_size);
        stale_temporaries.push_back(iterator->path());
        continue;
      }

      const std::string hex =
          hex_from_filename(filename, layout_.file_suffix);
      const std::uintmax_t byte_size =
          iterator->file_size(iteration_error);
      if (iteration_error ||
          refreshed_count >= max_entry_count_ ||
          byte_size > max_single_stored_entry_bytes_ ||
          byte_size >
              static_cast<std::uintmax_t>(
                  max_total_stored_bytes_ -
                  refreshed_stored_bytes)) {
        fail("persistent durable retention repository exceeds bounds");
      }
      const auto bytes = posix::read_exact_regular_file(
          iterator->path(), max_single_stored_entry_bytes_, false,
          kPosixErrors,
          kReadMessages);
      if (!bytes.has_value()) {
        throw std::logic_error(
            "enumerated durable retention entry disappeared");
      }
      const std::uint64_t logical_byte_size =
          verify_entry_bytes(kind_, hex, *bytes);
      if (refreshed_count >= max_entry_count_ ||
          logical_byte_size >
              max_total_bytes_ - refreshed_bytes ||
          static_cast<std::uint64_t>(bytes->size()) >
              max_total_stored_bytes_ - refreshed_stored_bytes) {
        fail("persistent durable retention repository exceeds bounds");
      }
      ++refreshed_count;
      refreshed_bytes += logical_byte_size;
      refreshed_stored_bytes +=
          static_cast<std::uint64_t>(bytes->size());
    }
    if (iteration_error) {
      throw std::filesystem::filesystem_error(
          "cannot enumerate durable retention repository",
          data_directory_, iteration_error);
    }

    if (remove_stale_temporaries) {
      for (const auto& temporary : stale_temporaries) {
        if (::unlink(temporary.c_str()) != 0) {
          fail_errno("cannot remove stale durable retention temporary file");
        }
      }
    }
    // Successful enumeration adopts every visible immutable final. The
    // unconditional barrier also closes the final-only crash residue window
    // when there is no stale temporary name left to clean.
    posix::sync_directory(data_directory_, kDirectorySyncMessages);
    entry_count_ = refreshed_count;
    retained_bytes_ = refreshed_bytes;
    stored_bytes_ = refreshed_stored_bytes;
  }

  DurableValueKind kind_;
  DurableStoreLayout layout_;
  std::filesystem::path root_;
  std::filesystem::path namespace_directory_;
  std::filesystem::path data_directory_;
  std::size_t max_entry_count_;
  std::uint64_t max_total_bytes_;
  std::uint64_t max_single_entry_bytes_;
  std::uint64_t max_entry_overhead_;
  std::uint64_t max_total_stored_bytes_;
  std::uint64_t max_single_stored_entry_bytes_;
  std::unique_ptr<posix::ProcessAffineLockFile> lock_;
  std::size_t entry_count_{0U};
  std::uint64_t retained_bytes_{0U};
  std::uint64_t stored_bytes_{0U};
  std::mutex mutex_;
};

}  // namespace

struct FilesystemRawArtifactStore::Storage final {
  Storage(std::filesystem::path trusted_root,
          std::size_t max_blob_count, std::uint64_t max_total_bytes,
          std::uint64_t max_single_blob_bytes)
      : files(DurableValueKind::kRawArtifact, std::move(trusted_root),
              max_blob_count, max_total_bytes, max_single_blob_bytes) {}

  DurableFileCollection files;
};

FilesystemRawArtifactStore::FilesystemRawArtifactStore(
    std::filesystem::path trusted_root, std::size_t max_blob_count,
    std::uint64_t max_total_bytes, std::uint64_t max_single_blob_bytes)
    : storage_(std::make_unique<Storage>(
          std::move(trusted_root), max_blob_count, max_total_bytes,
          max_single_blob_bytes)) {}

FilesystemRawArtifactStore::~FilesystemRawArtifactStore() = default;

StoredRawArtifact FilesystemRawArtifactStore::put_if_absent(
    std::string_view bytes) {
  storage_->files.require_insert_size(bytes.size());
  const std::string digest = sha256_digest(bytes);
  const auto hex = detail::retained_digest_hex(digest);
  if (!hex.has_value()) {
    throw std::logic_error("SHA-256 helper returned an invalid digest");
  }
  const bool inserted = storage_->files.put_if_absent(*hex, bytes);
  return {
      core::ContentDigest(digest),
      core::ContentAddressedStoreKey(
          detail::retained_store_key_for_digest(digest)),
      static_cast<std::uint64_t>(bytes.size()),
      inserted,
  };
}

std::optional<std::string> FilesystemRawArtifactStore::get_verified(
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& expected_digest,
    std::uint64_t expected_size) const {
  storage_->files.require_owner_process();
  const auto hex =
      detail::retained_raw_key_hex(key, expected_digest);
  if (!hex.has_value()) {
    return std::nullopt;
  }
  const auto bytes = storage_->files.get_verified(*hex);
  if (!bytes.has_value() ||
      static_cast<std::uint64_t>(bytes->size()) != expected_size ||
      sha256_digest(*bytes) != expected_digest.value()) {
    return std::nullopt;
  }
  return bytes;
}

std::size_t FilesystemRawArtifactStore::size() const {
  return storage_->files.size();
}

std::uint64_t FilesystemRawArtifactStore::retained_bytes() const {
  return storage_->files.retained_bytes();
}

struct FilesystemCatalogArtifactStore::Storage final {
  Storage(std::filesystem::path trusted_root,
          std::size_t max_artifact_count,
          std::uint64_t max_total_payload_bytes,
          std::uint64_t max_single_payload_bytes)
      : files(DurableValueKind::kCatalogArtifact,
              std::move(trusted_root), max_artifact_count,
              max_total_payload_bytes, max_single_payload_bytes,
              detail::kMaximumCatalogArtifactEnvelopeOverhead) {}

  DurableFileCollection files;
};

FilesystemCatalogArtifactStore::FilesystemCatalogArtifactStore(
    std::filesystem::path trusted_root,
    std::size_t max_artifact_count,
    std::uint64_t max_total_payload_bytes,
    std::uint64_t max_single_payload_bytes)
    : storage_(std::make_unique<Storage>(
          std::move(trusted_root), max_artifact_count,
          max_total_payload_bytes, max_single_payload_bytes)) {}

FilesystemCatalogArtifactStore::~FilesystemCatalogArtifactStore() = default;

StoredCatalogArtifact FilesystemCatalogArtifactStore::put_if_absent(
    const core::ContentAddressedStoreKey& exact_key,
    const core::ContentDigest& expected_digest,
    std::string_view bytes) {
  storage_->files.require_insert_size(bytes.size());
  const auto storage_hex =
      detail::catalog_artifact_storage_hex(exact_key.value());
  if (!storage_hex.has_value()) {
    fail("catalog artifact key must be non-empty and within its fixed "
         "bound");
  }
  const std::string envelope =
      detail::encode_catalog_artifact_envelope(
          exact_key.value(), expected_digest.value(), bytes);
  const bool inserted =
      storage_->files.put_if_absent(*storage_hex, envelope);
  return {
      exact_key,
      expected_digest,
      static_cast<std::uint64_t>(bytes.size()),
      inserted,
  };
}

std::optional<std::string>
FilesystemCatalogArtifactStore::get_verified(
    const core::ContentAddressedStoreKey& exact_key,
    const core::ContentDigest& expected_digest,
    std::optional<std::uint64_t> expected_size) const {
  storage_->files.require_owner_process();
  const auto storage_hex =
      detail::catalog_artifact_storage_hex(exact_key.value());
  if (!storage_hex.has_value() ||
      !is_sha256_digest(expected_digest.value())) {
    return std::nullopt;
  }
  const auto envelope = storage_->files.get_verified(*storage_hex);
  if (!envelope.has_value()) {
    return std::nullopt;
  }
  const auto decoded =
      detail::decode_catalog_artifact_envelope(*envelope);
  if (decoded.content_addressed_store_key != exact_key.value()) {
    throw std::runtime_error(
        "durable catalog-artifact filename collision");
  }
  if (decoded.content_digest != expected_digest.value() ||
      (expected_size.has_value() &&
       decoded.payload.size() != *expected_size)) {
    return std::nullopt;
  }
  return std::string(decoded.payload);
}

std::size_t FilesystemCatalogArtifactStore::size() const {
  return storage_->files.size();
}

std::uint64_t FilesystemCatalogArtifactStore::retained_bytes() const {
  return storage_->files.retained_bytes();
}

struct FilesystemRetainedBundleRepository::Storage final {
  Storage(std::filesystem::path trusted_root,
          std::size_t max_bundle_count, std::uint64_t max_total_bytes,
          std::uint64_t max_single_bundle_bytes)
      : files(DurableValueKind::kRetainedBundle, std::move(trusted_root),
              max_bundle_count, max_total_bytes,
              max_single_bundle_bytes) {}

  DurableFileCollection files;
};

FilesystemRetainedBundleRepository::
    FilesystemRetainedBundleRepository(
        std::filesystem::path trusted_root,
        std::size_t max_bundle_count, std::uint64_t max_total_bytes,
        std::uint64_t max_single_bundle_bytes)
    : storage_(std::make_unique<Storage>(
          std::move(trusted_root), max_bundle_count, max_total_bytes,
          max_single_bundle_bytes)) {}

FilesystemRetainedBundleRepository::
    ~FilesystemRetainedBundleRepository() = default;

bool FilesystemRetainedBundleRepository::put_if_absent(
    std::shared_ptr<const RetainedBundle> bundle) {
  storage_->files.require_owner_process();
  if (bundle == nullptr) {
    fail("durable retained-bundle repository rejects null");
  }
  storage_->files.require_insert_size(
      bundle->canonical_bytes().size());
  if (!verify_retained_bundle_identity(*bundle)) {
    fail("durable retained-bundle repository rejects an invalid semantic "
         "identity");
  }
  const auto reloaded =
      detail::RetainedBundleReloadAuthority::reload(
          bundle->canonical_bytes());
  if (reloaded->retained_bundle_id() != bundle->retained_bundle_id() ||
      reloaded->value() != bundle->value()) {
    fail("durable retained-bundle repository rejects divergent typed and "
         "canonical values");
  }
  const auto hex =
      detail::retained_digest_hex(bundle->retained_bundle_id().value());
  if (!hex.has_value()) {
    fail("durable retained bundle has an invalid strong identity");
  }
  return storage_->files.put_if_absent(
      *hex, bundle->canonical_bytes());
}

std::shared_ptr<const RetainedBundle>
FilesystemRetainedBundleRepository::get_verified_envelope(
    const core::RetainedBundleId& retained_bundle_id) const {
  storage_->files.require_owner_process();
  const auto hex =
      detail::retained_digest_hex(retained_bundle_id.value());
  if (!hex.has_value()) {
    return nullptr;
  }
  const auto bytes = storage_->files.get_verified(*hex);
  if (!bytes.has_value()) {
    return nullptr;
  }
  const auto bundle =
      detail::RetainedBundleReloadAuthority::reload(*bytes);
  if (bundle->retained_bundle_id() != retained_bundle_id) {
    throw std::runtime_error(
        "durable retained-bundle repository key/content mismatch");
  }
  return bundle;
}

std::size_t FilesystemRetainedBundleRepository::size() const {
  return storage_->files.size();
}

std::uint64_t
FilesystemRetainedBundleRepository::retained_bytes() const {
  return storage_->files.retained_bytes();
}

struct FilesystemProgramBindingRepository::Storage final {
  Storage(std::filesystem::path trusted_root,
          std::size_t max_binding_count, std::uint64_t max_total_bytes,
          std::uint64_t max_single_binding_bytes)
      : files(DurableValueKind::kProgramBinding, std::move(trusted_root),
              max_binding_count, max_total_bytes,
              max_single_binding_bytes) {}

  DurableFileCollection files;
};

FilesystemProgramBindingRepository::FilesystemProgramBindingRepository(
    std::filesystem::path trusted_root,
    std::size_t max_binding_count, std::uint64_t max_total_bytes,
    std::uint64_t max_single_binding_bytes)
    : storage_(std::make_unique<Storage>(
          std::move(trusted_root), max_binding_count, max_total_bytes,
          max_single_binding_bytes)) {}

FilesystemProgramBindingRepository::~FilesystemProgramBindingRepository() =
    default;

bool FilesystemProgramBindingRepository::put_if_absent(
    std::shared_ptr<const ProgramBinding> binding,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map) {
  storage_->files.require_owner_process();
  if (binding == nullptr) {
    throw ProgramBindingError(
        "durable Program-Binding repository rejects null");
  }
  storage_->files.require_insert_size(binding->canonical_bytes().size());
  if (!verify_program_binding_identity(*binding)) {
    throw ProgramBindingError(
        "durable Program-Binding repository rejects an invalid identity");
  }
  const auto reloaded = reload_verified_program_binding(
      binding->canonical_bytes(), retained_bundle, retained_model_map);
  if (reloaded->program_binding_id() != binding->program_binding_id() ||
      reloaded->value() != binding->value()) {
    throw ProgramBindingError(
        "durable Program-Binding repository rejects divergent typed and "
        "canonical values");
  }
  const auto hex =
      detail::retained_digest_hex(binding->program_binding_id().value());
  if (!hex.has_value()) {
    throw ProgramBindingError(
        "durable Program Binding has an invalid strong identity");
  }
  return storage_->files.put_if_absent(*hex, binding->canonical_bytes());
}

std::shared_ptr<const ProgramBinding>
FilesystemProgramBindingRepository::get_verified(
    const core::ProgramBindingId& program_binding_id,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map) const {
  storage_->files.require_owner_process();
  const auto hex =
      detail::retained_digest_hex(program_binding_id.value());
  if (!hex.has_value()) {
    return nullptr;
  }
  const auto bytes = storage_->files.get_verified(*hex);
  if (!bytes.has_value()) {
    return nullptr;
  }
  const auto binding = reload_verified_program_binding(
      *bytes, retained_bundle, retained_model_map);
  if (binding->program_binding_id() != program_binding_id) {
    throw std::runtime_error(
        "durable Program-Binding repository key/content mismatch");
  }
  return binding;
}

std::size_t FilesystemProgramBindingRepository::size() const {
  return storage_->files.size();
}

std::uint64_t
FilesystemProgramBindingRepository::retained_bytes() const {
  return storage_->files.retained_bytes();
}

}  // namespace manyforge::deployment
