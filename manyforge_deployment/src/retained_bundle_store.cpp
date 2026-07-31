#include "manyforge_deployment/retained_bundle.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

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
#include <variant>
#include <vector>

#include "manyforge_deployment/content_identity.hpp"
#include "retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

enum class DurableValueKind {
  kRawArtifact,
  kRetainedBundle,
};

struct DurableStoreLayout final {
  std::string_view directory_name;
  std::string_view file_suffix;
};

[[nodiscard]] constexpr DurableStoreLayout layout_for(
    DurableValueKind kind) noexcept {
  switch (kind) {
    case DurableValueKind::kRawArtifact:
      return {"raw-artifacts", ".blob"};
    case DurableValueKind::kRetainedBundle:
      return {"retained-bundles", ".json"};
  }
  return {};
}

constexpr std::size_t kMaximumStaleTemporaryCount = 256U;
constexpr std::size_t kDigestHexLength = 64U;
constexpr std::string_view kDigestPrefix = "sha256:";
constexpr std::string_view kStoreKeyPrefix = "sha256/";

[[noreturn]] void fail(std::string_view message) {
  throw RetainedBundleError(std::string(message));
}

[[noreturn]] void fail_errno(std::string_view operation) {
  throw std::runtime_error(std::string(operation) + ": " +
                           std::strerror(errno));
}

[[nodiscard]] bool is_lower_hex(std::string_view value) noexcept {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char byte) {
           return (byte >= '0' && byte <= '9') ||
                  (byte >= 'a' && byte <= 'f');
         });
}

[[nodiscard]] std::optional<std::string_view> digest_hex(
    std::string_view digest) noexcept {
  if (!is_sha256_digest(digest)) {
    return std::nullopt;
  }
  return digest.substr(kDigestPrefix.size());
}

[[nodiscard]] std::string digest_from_hex(std::string_view hex) {
  if (hex.size() != kDigestHexLength || !is_lower_hex(hex)) {
    fail("durable retention entry has an invalid digest filename");
  }
  return std::string(kDigestPrefix) + std::string(hex);
}

[[nodiscard]] std::filesystem::path absolute_trusted_root(
    const std::filesystem::path& root) {
  if (root.empty()) {
    fail("durable retention trusted root must not be empty");
  }
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(root, error);
  if (error) {
    throw std::filesystem::filesystem_error(
        "cannot resolve durable retention trusted root", root, error);
  }
  absolute = absolute.lexically_normal();
  while (absolute != absolute.root_path() &&
         absolute.filename().empty()) {
    absolute = absolute.parent_path();
  }
  const auto status = std::filesystem::symlink_status(absolute, error);
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    fail("durable retention trusted root must be an existing plain "
         "directory");
  }
  return absolute;
}

[[nodiscard]] bool ensure_plain_directory(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::filesystem::filesystem_error(
        "cannot inspect durable retention directory", path, error);
  }
  if (std::filesystem::exists(status)) {
    if (!std::filesystem::is_directory(status) ||
        std::filesystem::is_symlink(status)) {
      fail("durable retention path is not a plain directory");
    }
    return false;
  }
  const bool created = std::filesystem::create_directory(path, error);
  if (error) {
    throw std::filesystem::filesystem_error(
        "cannot create durable retention directory", path, error);
  }
  if (!created) {
    const auto raced_status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_directory(raced_status) ||
        std::filesystem::is_symlink(raced_status)) {
      fail("concurrently created durable retention path is unsafe");
    }
    return false;
  }
  const auto created_status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_directory(created_status) ||
      std::filesystem::is_symlink(created_status)) {
    fail("created durable retention directory is unsafe");
  }
  return true;
}

void fsync_directory(const std::filesystem::path& directory) {
  const int fd =
      ::open(directory.c_str(),
             O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (fd < 0) {
    fail_errno("cannot open durable retention directory for fsync");
  }
  const int result = ::fsync(fd);
  const int saved_errno = errno;
  (void)::close(fd);
  if (result != 0) {
    errno = saved_errno;
    fail_errno("cannot fsync durable retention directory");
  }
}

class AdvisoryFileLock final {
 public:
  AdvisoryFileLock(int fd, int operation) : fd_(fd) {
    if (::flock(fd_, operation) != 0) {
      fail_errno("cannot lock durable retention store");
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
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (missing_is_empty && errno == ENOENT) {
      return std::nullopt;
    }
    fail_errno("cannot open durable retention entry");
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
    fail_errno("cannot stat durable retention entry");
  }
  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > maximum_bytes ||
      static_cast<std::uint64_t>(status.st_size) >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    fail("durable retention entry violates its immutable byte bound");
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
      fail("durable retention entry changed during exact read");
    }
    offset += static_cast<std::size_t>(read_count);
  }
  char extra = '\0';
  ssize_t extra_count = 0;
  do {
    extra_count = ::read(fd, &extra, 1U);
  } while (extra_count < 0 && errno == EINTR);
  if (extra_count != 0) {
    fail("durable retention entry grew during exact read");
  }
  return bytes;
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
      fail_errno("cannot write durable retention temporary file");
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(fd) != 0) {
    fail_errno("cannot fsync durable retention temporary file");
  }
}

[[nodiscard]] bool is_stale_temporary_filename(
    std::string_view filename, std::string_view suffix) noexcept {
  if (!filename.starts_with('.') ||
      filename.size() <= 1U + kDigestHexLength + suffix.size()) {
    return false;
  }
  const std::string_view hex = filename.substr(1U, kDigestHexLength);
  const std::string_view remainder =
      filename.substr(1U + kDigestHexLength);
  return is_lower_hex(hex) && remainder.starts_with(suffix) &&
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
  (void)digest_from_hex(hex);
  return std::string(hex);
}

void verify_entry_bytes(DurableValueKind kind, std::string_view hex,
                        std::string_view bytes) {
  const std::string expected_digest = digest_from_hex(hex);
  switch (kind) {
    case DurableValueKind::kRawArtifact:
      if (sha256_digest(bytes) != expected_digest) {
        fail("durable raw-artifact filename does not match its bytes");
      }
      return;
    case DurableValueKind::kRetainedBundle: {
      const auto bundle =
          detail::RetainedBundleReloadAuthority::reload(bytes);
      if (bundle->retained_bundle_id().value() != expected_digest) {
        fail("durable retained-bundle filename does not match its "
             "semantic identity");
      }
      return;
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
                        std::uint64_t max_single_entry_bytes)
      : kind_(kind),
        layout_(layout_for(kind)),
        root_(absolute_trusted_root(trusted_root)),
        namespace_directory_(
            root_ / std::string(layout_.directory_name)),
        data_directory_(namespace_directory_ / "sha256"),
        max_entry_count_(max_entry_count),
        max_total_bytes_(max_total_bytes),
        max_single_entry_bytes_(max_single_entry_bytes),
        owner_pid_(::getpid()) {
    if (max_entry_count_ == 0U || max_total_bytes_ == 0U ||
        max_single_entry_bytes_ == 0U ||
        max_single_entry_bytes_ > max_total_bytes_) {
      fail("durable retention limits must be positive and internally "
           "consistent");
    }

    (void)ensure_plain_directory(namespace_directory_);
    fsync_directory(root_);
    (void)ensure_plain_directory(data_directory_);
    fsync_directory(namespace_directory_);
    const std::filesystem::path lock_path =
        namespace_directory_ / ".repository.lock";
    lock_fd_ = ::open(lock_path.c_str(),
                      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd_ < 0) {
      fail_errno("cannot open durable retention repository lock");
    }
    try {
      struct stat lock_status {};
      if (::fstat(lock_fd_, &lock_status) != 0) {
        fail_errno("cannot stat durable retention repository lock");
      }
      if (!S_ISREG(lock_status.st_mode)) {
        fail("durable retention repository lock is not a plain file");
      }
      fsync_directory(namespace_directory_);

      std::lock_guard process_lock(mutex_);
      const AdvisoryFileLock file_lock(lock_fd_, LOCK_EX);
      refresh_index_locked(true);
    } catch (...) {
      const int saved_errno = errno;
      (void)::close(lock_fd_);
      lock_fd_ = -1;
      errno = saved_errno;
      throw;
    }
  }

  ~DurableFileCollection() {
    if (lock_fd_ >= 0) {
      (void)::close(lock_fd_);
    }
  }

  DurableFileCollection(const DurableFileCollection&) = delete;
  DurableFileCollection& operator=(const DurableFileCollection&) = delete;

  void require_owner_process() const {
    if (::getpid() != owner_pid_) {
      fail("durable retention store instances must be reopened after fork");
    }
  }

  void require_insert_size(std::size_t byte_size) const {
    require_owner_process();
    if (byte_size > max_single_entry_bytes_) {
      fail("durable retention value exceeds its single-entry bound");
    }
  }

  [[nodiscard]] bool put_if_absent(std::string_view hex,
                                   std::string_view bytes) {
    require_insert_size(bytes.size());
    (void)digest_from_hex(hex);
    verify_entry_bytes(kind_, hex, bytes);

    std::lock_guard process_lock(mutex_);
    const AdvisoryFileLock file_lock(lock_fd_, LOCK_EX);
    refresh_index_locked(true);
    const std::filesystem::path final_path =
        data_directory_ / committed_filename(hex);
    if (const auto existing =
            read_plain_file(final_path, max_single_entry_bytes_, true);
        existing.has_value()) {
      verify_entry_bytes(kind_, hex, *existing);
      if (*existing != bytes) {
        throw std::runtime_error(
            "durable retention identity collision");
      }
      return false;
    }

    const std::uint64_t byte_size =
        static_cast<std::uint64_t>(bytes.size());
    if (entry_count_ >= max_entry_count_ ||
        byte_size > max_total_bytes_ - retained_bytes_) {
      fail("durable retention repository bound is exhausted");
    }

    static std::atomic<std::uint64_t> sequence{0U};
    std::filesystem::path temporary_path;
    int temporary_fd = -1;
    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
      std::string temporary_name{"."};
      temporary_name.append(committed_filename(hex));
      temporary_name.append(".tmp.");
      temporary_name.append(
          std::to_string(static_cast<long long>(::getpid())));
      temporary_name.push_back('.');
      temporary_name.append(std::to_string(sequence.fetch_add(1U)));
      temporary_path = data_directory_ / temporary_name;
      temporary_fd = ::open(
          temporary_path.c_str(),
          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (temporary_fd >= 0) {
        break;
      }
      if (errno != EEXIST) {
        fail_errno("cannot create durable retention temporary file");
      }
    }
    if (temporary_fd < 0) {
      fail("cannot allocate a unique durable retention temporary file");
    }

    try {
      write_all_and_fsync(temporary_fd, bytes);
      if (::close(temporary_fd) != 0) {
        temporary_fd = -1;
        fail_errno("cannot close durable retention temporary file");
      }
      temporary_fd = -1;
      if (::link(temporary_path.c_str(), final_path.c_str()) != 0) {
        if (errno != EEXIST) {
          fail_errno("cannot publish durable retention value atomically");
        }
        const auto existing =
            read_plain_file(final_path, max_single_entry_bytes_, false);
        if (!existing.has_value() || *existing != bytes) {
          throw std::runtime_error(
              "durable retention identity collision");
        }
        verify_entry_bytes(kind_, hex, *existing);
        if (::unlink(temporary_path.c_str()) != 0) {
          fail_errno(
              "cannot remove idempotent durable retention temporary file");
        }
        fsync_directory(data_directory_);
        return false;
      }
      if (::unlink(temporary_path.c_str()) != 0) {
        fail_errno("cannot remove durable retention temporary link");
      }
      fsync_directory(data_directory_);
    } catch (...) {
      const int saved_errno = errno;
      if (temporary_fd >= 0) {
        (void)::close(temporary_fd);
      }
      (void)::unlink(temporary_path.c_str());
      errno = saved_errno;
      throw;
    }
    ++entry_count_;
    retained_bytes_ += byte_size;
    return true;
  }

  [[nodiscard]] std::optional<std::string> get_verified(
      std::string_view hex) {
    require_owner_process();
    (void)digest_from_hex(hex);
    std::lock_guard process_lock(mutex_);
    const AdvisoryFileLock file_lock(lock_fd_, LOCK_SH);
    const auto bytes = read_plain_file(
        data_directory_ / committed_filename(hex),
        max_single_entry_bytes_, true);
    if (!bytes.has_value()) {
      return std::nullopt;
    }
    verify_entry_bytes(kind_, hex, *bytes);
    // A successful point read adopts a visible immutable final left by a
    // writer that may have died after link + temporary unlink but before its
    // directory fsync.
    fsync_directory(data_directory_);
    return bytes;
  }

  [[nodiscard]] std::size_t size() {
    require_owner_process();
    std::lock_guard process_lock(mutex_);
    const AdvisoryFileLock file_lock(lock_fd_, LOCK_SH);
    refresh_index_locked(false);
    return entry_count_;
  }

  [[nodiscard]] std::uint64_t retained_bytes() {
    require_owner_process();
    std::lock_guard process_lock(mutex_);
    const AdvisoryFileLock file_lock(lock_fd_, LOCK_SH);
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
        if (iteration_error || byte_size > max_single_entry_bytes_ ||
            ++stale_count > maximum_stale_count ||
            static_cast<std::uint64_t>(byte_size) >
                max_total_bytes_ - stale_bytes) {
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
          byte_size > max_single_entry_bytes_ ||
          byte_size >
              static_cast<std::uintmax_t>(
                  max_total_bytes_ - refreshed_bytes)) {
        fail("persistent durable retention repository exceeds bounds");
      }
      const auto bytes =
          read_plain_file(iterator->path(), max_single_entry_bytes_, false);
      if (!bytes.has_value()) {
        throw std::logic_error(
            "enumerated durable retention entry disappeared");
      }
      if (refreshed_count >= max_entry_count_ ||
          static_cast<std::uint64_t>(bytes->size()) >
              max_total_bytes_ - refreshed_bytes) {
        fail("persistent durable retention repository exceeds bounds");
      }
      verify_entry_bytes(kind_, hex, *bytes);
      ++refreshed_count;
      refreshed_bytes += static_cast<std::uint64_t>(bytes->size());
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
    fsync_directory(data_directory_);
    entry_count_ = refreshed_count;
    retained_bytes_ = refreshed_bytes;
  }

  DurableValueKind kind_;
  DurableStoreLayout layout_;
  std::filesystem::path root_;
  std::filesystem::path namespace_directory_;
  std::filesystem::path data_directory_;
  std::size_t max_entry_count_;
  std::uint64_t max_total_bytes_;
  std::uint64_t max_single_entry_bytes_;
  pid_t owner_pid_;
  int lock_fd_{-1};
  std::size_t entry_count_{0U};
  std::uint64_t retained_bytes_{0U};
  std::mutex mutex_;
};

[[nodiscard]] std::optional<std::string_view> raw_key_hex(
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& expected_digest) noexcept {
  const auto hex = digest_hex(expected_digest.value());
  if (!hex.has_value() ||
      key.value() != std::string(kStoreKeyPrefix) + std::string(*hex)) {
    return std::nullopt;
  }
  return hex;
}

void require_verified_raw_reference(
    DurableFileCollection& files,
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& digest,
    std::optional<std::uint64_t> expected_size,
    std::string_view reference_label) {
  const auto hex = raw_key_hex(key, digest);
  if (!hex.has_value()) {
    fail(std::string(reference_label) +
         " has an inconsistent digest-derived raw-CAS key");
  }
  const auto bytes = files.get_verified(*hex);
  if (!bytes.has_value()) {
    fail(std::string(reference_label) +
         " is absent from the durable raw-artifact CAS");
  }
  if ((expected_size.has_value() &&
       static_cast<std::uint64_t>(bytes->size()) != *expected_size) ||
      sha256_digest(*bytes) != digest.value()) {
    fail(std::string(reference_label) +
         " failed exact durable raw-artifact verification");
  }
}

void require_verified_product_artifacts(
    DurableFileCollection& files,
    const core::RetainedProductDTO& product) {
  for (const auto& artifact : product.artifact_refs) {
    require_verified_raw_reference(
        files, artifact.content_addressed_store_key,
        artifact.content_digest, artifact.byte_size,
        "retained product artifact reference");
  }
}

void require_verified_bundle_references(
    DurableFileCollection& files, const RetainedBundle& bundle) {
  const auto& value = bundle.value();
  for (const auto& record : value.catalogs.records) {
    std::visit(
        [&files](const auto& typed_record) {
          require_verified_raw_reference(
              files, typed_record.content_addressed_store_key,
              typed_record.content_digest, std::nullopt,
              "retained catalog-record reference");
        },
        record);
  }
  for (const auto& schema : value.catalogs.schema_artifacts) {
    require_verified_raw_reference(
        files, schema.content_addressed_store_key,
        schema.content_digest, std::nullopt,
        "retained schema-artifact reference");
  }

  const auto& products = value.compiled_products;
  require_verified_product_artifacts(
      files, products.planning_collision_model);
  if (products.viewer_model.has_value()) {
    require_verified_product_artifacts(files, *products.viewer_model);
  }
  for (const auto& group : products.control_domain_products) {
    for (const auto& product : group.products) {
      require_verified_product_artifacts(files, product);
    }
  }
  for (const auto& product : products.symbol_role_maps) {
    require_verified_product_artifacts(files, product);
  }
  for (const auto& group : products.runtime_unit_products) {
    for (const auto& product : group.products) {
      require_verified_product_artifacts(files, product);
    }
  }
}

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
  const auto hex = digest_hex(digest);
  if (!hex.has_value()) {
    throw std::logic_error("SHA-256 helper returned an invalid digest");
  }
  const bool inserted = storage_->files.put_if_absent(*hex, bytes);
  return {
      core::ContentDigest(digest),
      core::ContentAddressedStoreKey(
          std::string(kStoreKeyPrefix) + std::string(*hex)),
      static_cast<std::uint64_t>(bytes.size()),
      inserted,
  };
}

std::optional<std::string> FilesystemRawArtifactStore::get_verified(
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& expected_digest,
    std::uint64_t expected_size) const {
  storage_->files.require_owner_process();
  const auto hex = raw_key_hex(key, expected_digest);
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
  const auto hex = digest_hex(bundle->retained_bundle_id().value());
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
  const auto hex = digest_hex(retained_bundle_id.value());
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

FilesystemVerifiedRetainedBundleSource::
    FilesystemVerifiedRetainedBundleSource(
        const FilesystemRetainedBundleRepository& bundle_repository,
        const FilesystemRawArtifactStore& artifact_store)
    : bundle_repository_(bundle_repository),
      artifact_store_(artifact_store) {
  if (bundle_repository_.storage_->files.trusted_root() !=
      artifact_store_.storage_->files.trusted_root()) {
    fail("verified retained-bundle source rejects repository/raw-CAS "
         "trusted-root splicing");
  }
}

std::shared_ptr<const RetainedBundle>
FilesystemVerifiedRetainedBundleSource::get_verified(
    const core::RetainedBundleId& retained_bundle_id) const {
  bundle_repository_.storage_->files.require_owner_process();
  artifact_store_.storage_->files.require_owner_process();
  const auto bundle =
      bundle_repository_.get_verified_envelope(retained_bundle_id);
  if (bundle == nullptr) {
    return nullptr;
  }
  require_verified_bundle_references(
      artifact_store_.storage_->files, *bundle);
  return bundle;
}

}  // namespace manyforge::deployment
