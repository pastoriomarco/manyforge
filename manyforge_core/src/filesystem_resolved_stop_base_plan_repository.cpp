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
#include <functional>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/filesystem_resolved_stop_base_plan_repository.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

constexpr std::string_view kRepositoryDirectoryName = "resolved-stop-base-plans";
constexpr std::string_view kDataDirectoryName = "sha256";
constexpr std::string_view kLockFilename = ".repository.lock";
constexpr std::string_view kFinalSuffix = ".json";
constexpr std::string_view kTemporaryMarker = ".json.tmp.";
constexpr std::size_t kDigestHexLength = 64U;
constexpr std::size_t kMaximumStaleTemporaryCount = 256U;

class RepositoryFailure final : public std::runtime_error {
 public:
  explicit RepositoryFailure(ErrorDTO error)
      : std::runtime_error(error.message), error_(std::move(error)) {}

  [[nodiscard]] const ErrorDTO& error() const noexcept { return error_; }

 private:
  ErrorDTO error_;
};

[[noreturn]] void fail(std::string code, std::string message,
                       std::map<std::string, std::string> details) {
  throw RepositoryFailure(ErrorDTO{std::move(code), std::move(message), std::move(details)});
}

[[noreturn]] void fail_integrity(std::string reason) {
  fail("resolved_stop_base_plan_repository_integrity_failure",
       "the resolved Stop base-plan repository failed closed integrity verification",
       {{"reason", std::move(reason)}});
}

[[noreturn]] void fail_storage(std::string_view operation) {
  const int saved_errno = errno;
  fail("resolved_stop_base_plan_repository_storage_failure",
       std::string(operation) + ": " + std::strerror(saved_errno),
       {{"reason", "filesystem_operation_failed"}, {"operation", std::string(operation)}});
}

[[nodiscard]] bool is_lower_hex(std::string_view value) noexcept {
  return value.size() == kDigestHexLength &&
         std::all_of(value.begin(), value.end(), [](const unsigned char byte) {
           return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
         });
}

[[nodiscard]] std::string digest_hex(const ResolvedStopBasePlanId& resolved_stop_base_plan_id) {
  const std::string& value = resolved_stop_base_plan_id.value();
  if (!content_identity::is_sha256_digest(value)) {
    fail("invalid_request", "resolved Stop base-plan ID is malformed",
         {{"reason", "resolved_stop_base_plan_id_invalid"}, {"resolved_stop_base_plan_id", value}});
  }
  return value.substr(std::string_view("sha256:").size());
}

[[nodiscard]] ResolvedStopBasePlanId id_from_filename(std::string_view filename) {
  if (filename.size() != kDigestHexLength + kFinalSuffix.size() ||
      !filename.ends_with(kFinalSuffix)) {
    fail_integrity("unknown_repository_entry");
  }
  const std::string_view hex = filename.substr(0U, kDigestHexLength);
  if (!is_lower_hex(hex)) {
    fail_integrity("invalid_repository_entry_identity");
  }
  return ResolvedStopBasePlanId("sha256:" + std::string(hex));
}

[[nodiscard]] bool is_stale_temporary_filename(std::string_view filename) noexcept {
  if (!filename.starts_with('.') ||
      filename.size() <= 1U + kDigestHexLength + kTemporaryMarker.size()) {
    return false;
  }
  const std::string_view hex = filename.substr(1U, kDigestHexLength);
  const std::string_view remainder = filename.substr(1U + kDigestHexLength);
  return is_lower_hex(hex) && remainder.starts_with(kTemporaryMarker) &&
         remainder.size() > kTemporaryMarker.size();
}

[[nodiscard]] std::filesystem::path normalize_trusted_root(const std::filesystem::path& root) {
  if (root.empty()) {
    fail("resolved_stop_base_plan_repository_invalid_configuration",
         "resolved Stop base-plan repository root is empty", {{"reason", "trusted_root_empty"}});
  }
  std::error_code error;
  auto absolute = std::filesystem::absolute(root, error);
  if (error) {
    fail("resolved_stop_base_plan_repository_invalid_configuration",
         "cannot resolve resolved Stop base-plan repository root",
         {{"reason", "trusted_root_resolution_failed"}});
  }
  absolute = absolute.lexically_normal();
  while (absolute != absolute.root_path() && absolute.filename().empty()) {
    absolute = absolute.parent_path();
  }
  const auto status = std::filesystem::symlink_status(absolute, error);
  if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
    fail("resolved_stop_base_plan_repository_invalid_configuration",
         "resolved Stop base-plan repository root must be an existing plain directory",
         {{"reason", "trusted_root_not_plain_directory"}});
  }
  return absolute;
}

void sync_directory(const std::filesystem::path& directory) {
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (descriptor < 0) {
    fail_storage("open resolved Stop base-plan directory for fsync");
  }
  const int result = ::fsync(descriptor);
  const int saved_errno = errno;
  (void)::close(descriptor);
  if (result != 0) {
    errno = saved_errno;
    fail_storage("fsync resolved Stop base-plan directory");
  }
}

void ensure_owned_plain_directory(const std::filesystem::path& directory) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(directory, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    fail("resolved_stop_base_plan_repository_storage_failure",
         "cannot inspect resolved Stop base-plan repository directory",
         {{"reason", "repository_directory_inspection_failed"}});
  }
  if (std::filesystem::exists(status)) {
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
      fail_integrity("repository_path_not_plain_directory");
    }
  } else {
    const bool created = std::filesystem::create_directory(directory, error);
    if (error) {
      fail("resolved_stop_base_plan_repository_storage_failure",
           "cannot create resolved Stop base-plan repository directory",
           {{"reason", "repository_directory_creation_failed"}});
    }
    if (!created) {
      const auto raced_status = std::filesystem::symlink_status(directory, error);
      if (error || std::filesystem::is_symlink(raced_status) ||
          !std::filesystem::is_directory(raced_status)) {
        fail_integrity("concurrently_created_repository_path_not_plain");
      }
    }
  }
  sync_directory(directory.parent_path());
}

class ScopedAdvisoryLock final {
 public:
  ScopedAdvisoryLock(int descriptor, int operation) : descriptor_(descriptor) {
    while (::flock(descriptor_, operation) != 0) {
      if (errno == EINTR) {
        continue;
      }
      fail_storage("lock resolved Stop base-plan repository");
    }
  }

  ~ScopedAdvisoryLock() { (void)::flock(descriptor_, LOCK_UN); }

  ScopedAdvisoryLock(const ScopedAdvisoryLock&) = delete;
  ScopedAdvisoryLock& operator=(const ScopedAdvisoryLock&) = delete;
  ScopedAdvisoryLock(ScopedAdvisoryLock&&) = delete;
  ScopedAdvisoryLock& operator=(ScopedAdvisoryLock&&) = delete;

 private:
  int descriptor_;
};

class ProcessAffineLockFile final {
 public:
  explicit ProcessAffineLockFile(const std::filesystem::path& path) : owner_pid_(::getpid()) {
    descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor_ < 0) {
      fail_storage("open resolved Stop base-plan repository lock");
    }
    try {
      struct stat status {};
      if (::fstat(descriptor_, &status) != 0) {
        fail_storage("stat resolved Stop base-plan repository lock");
      }
      if (!S_ISREG(status.st_mode)) {
        fail_integrity("repository_lock_not_regular_file");
      }
      sync_directory(path.parent_path());
    } catch (...) {
      const int saved_errno = errno;
      (void)::close(descriptor_);
      descriptor_ = -1;
      errno = saved_errno;
      throw;
    }
  }

  ~ProcessAffineLockFile() {
    if (descriptor_ >= 0) {
      (void)::close(descriptor_);
    }
  }

  ProcessAffineLockFile(const ProcessAffineLockFile&) = delete;
  ProcessAffineLockFile& operator=(const ProcessAffineLockFile&) = delete;

  void require_owner_process() const {
    if (::getpid() != owner_pid_) {
      fail("resolved_stop_base_plan_repository_inherited_process",
           "resolved Stop base-plan repository must be reopened after fork",
           {{"reason", "repository_instance_inherited_after_fork"}});
    }
  }

  [[nodiscard]] ScopedAdvisoryLock acquire(int operation) const {
    require_owner_process();
    return ScopedAdvisoryLock(descriptor_, operation);
  }

 private:
  pid_t owner_pid_;
  int descriptor_{-1};
};

[[nodiscard]] bool same_file_snapshot(const struct stat& left, const struct stat& right) noexcept {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
         left.st_size == right.st_size && left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
         left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
         left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
         left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

[[nodiscard]] std::optional<std::string> read_exact_regular_file(const std::filesystem::path& path,
                                                                 std::uint64_t maximum_bytes,
                                                                 bool missing_is_empty) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) {
    if (missing_is_empty && errno == ENOENT) {
      return std::nullopt;
    }
    fail_storage("open resolved Stop base-plan entry");
  }
  struct ScopedClose final {
    int descriptor;
    ~ScopedClose() {
      if (descriptor >= 0) {
        (void)::close(descriptor);
      }
    }
  } close{descriptor};

  struct stat before {};
  if (::fstat(descriptor, &before) != 0) {
    fail_storage("stat resolved Stop base-plan entry");
  }
  if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
      static_cast<std::uint64_t>(before.st_size) > maximum_bytes ||
      static_cast<std::uint64_t>(before.st_size) >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    fail_integrity("entry_not_regular_or_exceeds_single_value_bound");
  }

  std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const ssize_t count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      fail_integrity("entry_changed_during_exact_read");
    }
    offset += static_cast<std::size_t>(count);
  }
  char extra = '\0';
  ssize_t extra_count = 0;
  do {
    extra_count = ::read(descriptor, &extra, 1U);
  } while (extra_count < 0 && errno == EINTR);
  if (extra_count != 0) {
    fail_integrity("entry_grew_during_exact_read");
  }
  struct stat after {};
  if (::fstat(descriptor, &after) != 0) {
    fail_storage("restat resolved Stop base-plan entry");
  }
  if (!S_ISREG(after.st_mode) || !same_file_snapshot(before, after)) {
    fail_integrity("entry_changed_during_exact_read");
  }
  return bytes;
}

[[nodiscard]] ResolvedStopBasePlanDTO decode_stored_plan(
    std::string_view bytes, const ResolvedStopBasePlanId& expected_id) {
  const auto decoded = DecodeResolvedStopBasePlan(bytes);
  if (!decoded.ok()) {
    std::map<std::string, std::string> details{
        {"reason", "stored_plan_decode_failed"},
        {"resolved_stop_base_plan_id", expected_id.value()},
        {"decoder_code", decoded.error().code},
    };
    const auto decoder_reason = decoded.error().details.find("reason");
    if (decoder_reason != decoded.error().details.end()) {
      details.emplace("decoder_reason", decoder_reason->second);
    }
    fail("resolved_stop_base_plan_repository_integrity_failure",
         "stored resolved Stop base-plan bytes are invalid", std::move(details));
  }
  if (decoded.value().resolved_stop_base_plan_id != expected_id) {
    fail("resolved_stop_base_plan_repository_integrity_failure",
         "stored resolved Stop base-plan filename does not match its content",
         {{"reason", "stored_plan_filename_identity_mismatch"},
          {"resolved_stop_base_plan_id", expected_id.value()},
          {"stored_resolved_stop_base_plan_id",
           decoded.value().resolved_stop_base_plan_id.value()}});
  }
  const auto encoded = EncodeResolvedStopBasePlan(decoded.value());
  if (!encoded.ok() || encoded.value() != bytes) {
    fail("resolved_stop_base_plan_repository_integrity_failure",
         "stored resolved Stop base-plan bytes do not reproduce canonically",
         {{"reason", "stored_plan_canonical_readback_mismatch"},
          {"resolved_stop_base_plan_id", expected_id.value()}});
  }
  return decoded.value();
}

void write_all_and_sync(int descriptor, std::string_view bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const ssize_t count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      fail_storage("write resolved Stop base-plan temporary entry");
    }
    offset += static_cast<std::size_t>(count);
  }
  if (::fsync(descriptor) != 0) {
    fail_storage("fsync resolved Stop base-plan temporary entry");
  }
}

using ExistingValueVerifier = std::function<void(std::string_view existing_bytes)>;

[[nodiscard]] bool publish_immutable_file_no_replace(const std::filesystem::path& final_path,
                                                     std::string_view bytes,
                                                     std::uint64_t maximum_existing_bytes,
                                                     const ExistingValueVerifier& verify_existing) {
  static std::atomic<std::uint64_t> sequence{0U};
  const std::filesystem::path directory = final_path.parent_path();
  std::filesystem::path temporary_path;
  int temporary_descriptor = -1;
  for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
    std::string temporary_name{"."};
    temporary_name.append(final_path.filename().string());
    temporary_name.append(".tmp.");
    temporary_name.append(std::to_string(static_cast<long long>(::getpid())));
    temporary_name.push_back('.');
    temporary_name.append(std::to_string(sequence.fetch_add(1U)));
    temporary_path = directory / temporary_name;
    temporary_descriptor =
        ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (temporary_descriptor >= 0) {
      break;
    }
    if (errno != EEXIST) {
      fail_storage("create resolved Stop base-plan temporary entry");
    }
  }
  if (temporary_descriptor < 0) {
    fail("resolved_stop_base_plan_repository_storage_failure",
         "cannot allocate a unique resolved Stop base-plan temporary entry",
         {{"reason", "temporary_name_exhausted"}});
  }

  try {
    write_all_and_sync(temporary_descriptor, bytes);
    if (::close(temporary_descriptor) != 0) {
      temporary_descriptor = -1;
      fail_storage("close resolved Stop base-plan temporary entry");
    }
    temporary_descriptor = -1;
    if (::link(temporary_path.c_str(), final_path.c_str()) != 0) {
      if (errno != EEXIST) {
        fail_storage("publish resolved Stop base-plan entry");
      }
      const auto existing = read_exact_regular_file(final_path, maximum_existing_bytes, false);
      if (!existing.has_value()) {
        fail_integrity("existing_publication_disappeared");
      }
      verify_existing(*existing);
      if (::unlink(temporary_path.c_str()) != 0) {
        fail_storage("remove idempotent resolved Stop base-plan temporary entry");
      }
      sync_directory(directory);
      return false;
    }
    if (::unlink(temporary_path.c_str()) != 0) {
      fail_storage("remove resolved Stop base-plan temporary link");
    }
    sync_directory(directory);
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

template <typename Value, typename Function>
[[nodiscard]] Result<Value> as_repository_result(Function&& function) {
  try {
    return Result<Value>::Ok(function());
  } catch (const RepositoryFailure& error) {
    return Result<Value>::Err(error.error());
  } catch (const std::exception& error) {
    return Result<Value>::Err(ErrorDTO{"resolved_stop_base_plan_repository_internal_failure",
                                       error.what(),
                                       {{"reason", "repository_exception"}}});
  }
}

}  // namespace

class FilesystemResolvedStopBasePlanRepository::Storage final {
 public:
  Storage(std::filesystem::path trusted_root, std::size_t maximum_plan_count,
          std::uint64_t maximum_total_bytes, std::uint64_t maximum_single_plan_bytes)
      : root_(normalize_trusted_root(trusted_root)),
        repository_directory_(root_ / std::string(kRepositoryDirectoryName)),
        data_directory_(repository_directory_ / std::string(kDataDirectoryName)),
        maximum_plan_count_(maximum_plan_count),
        maximum_total_bytes_(maximum_total_bytes),
        maximum_single_plan_bytes_(maximum_single_plan_bytes) {
    if (maximum_plan_count_ == 0U || maximum_total_bytes_ == 0U ||
        maximum_single_plan_bytes_ == 0U || maximum_single_plan_bytes_ > maximum_total_bytes_ ||
        maximum_single_plan_bytes_ > kMaximumResolvedStopBasePlanEncodedBytes) {
      fail("resolved_stop_base_plan_repository_invalid_configuration",
           "resolved Stop base-plan repository limits are invalid",
           {{"reason", "repository_bounds_invalid"}});
    }
    ensure_owned_plain_directory(repository_directory_);
    ensure_owned_plain_directory(data_directory_);
    lock_ =
        std::make_unique<ProcessAffineLockFile>(repository_directory_ / std::string(kLockFilename));
    std::lock_guard process_lock(mutex_);
    const auto file_lock = lock_->acquire(LOCK_EX);
    refresh_index_locked(true);
  }

  void require_owner_process() const { lock_->require_owner_process(); }

  void Put(const ResolvedStopBasePlanDTO& expected, std::string_view canonical_bytes) {
    require_owner_process();
    if (canonical_bytes.empty() || canonical_bytes.size() > maximum_single_plan_bytes_) {
      fail("resolved_stop_base_plan_repository_capacity_exhausted",
           "resolved Stop base plan exceeds the repository single-value bound",
           {{"reason", "single_plan_byte_bound_exhausted"},
            {"resolved_stop_base_plan_id", expected.resolved_stop_base_plan_id.value()}});
    }
    const auto input_readback =
        decode_stored_plan(canonical_bytes, expected.resolved_stop_base_plan_id);
    if (input_readback != expected) {
      fail_integrity("prepublication_dto_readback_mismatch");
    }

    std::lock_guard process_lock(mutex_);
    const auto file_lock = lock_->acquire(LOCK_EX);
    refresh_index_locked(true);
    const std::filesystem::path final_path = path_for(expected.resolved_stop_base_plan_id);
    if (const auto existing = read_exact_regular_file(final_path, maximum_single_plan_bytes_, true);
        existing.has_value()) {
      const auto existing_plan = decode_stored_plan(*existing, expected.resolved_stop_base_plan_id);
      if (*existing != canonical_bytes || existing_plan != expected) {
        fail("resolved_stop_base_plan_repository_identity_collision",
             "resolved Stop base-plan identity names different immutable bytes",
             {{"reason", "content_identity_collision"},
              {"resolved_stop_base_plan_id", expected.resolved_stop_base_plan_id.value()}});
      }
      sync_directory(data_directory_);
      return;
    }

    const auto byte_size = static_cast<std::uint64_t>(canonical_bytes.size());
    if (plan_count_ >= maximum_plan_count_ || byte_size > maximum_total_bytes_ - retained_bytes_) {
      fail("resolved_stop_base_plan_repository_capacity_exhausted",
           "resolved Stop base-plan repository capacity is exhausted",
           {{"reason", "repository_capacity_exhausted"},
            {"resolved_stop_base_plan_id", expected.resolved_stop_base_plan_id.value()}});
    }

    const bool inserted = publish_immutable_file_no_replace(
        final_path, canonical_bytes, maximum_single_plan_bytes_,
        [&](const std::string_view existing_bytes) {
          const auto existing_plan =
              decode_stored_plan(existing_bytes, expected.resolved_stop_base_plan_id);
          if (existing_bytes != canonical_bytes || existing_plan != expected) {
            fail("resolved_stop_base_plan_repository_identity_collision",
                 "resolved Stop base-plan identity names different immutable bytes",
                 {{"reason", "content_identity_collision"},
                  {"resolved_stop_base_plan_id", expected.resolved_stop_base_plan_id.value()}});
          }
        });
    if (inserted) {
      ++plan_count_;
      retained_bytes_ += byte_size;
    }

    // This is intentionally a new descriptor and a complete decoder pass.
    // Put never returns its input object as storage evidence.
    const auto stored_bytes =
        read_exact_regular_file(final_path, maximum_single_plan_bytes_, false);
    if (!stored_bytes.has_value()) {
      fail_integrity("published_plan_missing_during_independent_readback");
    }
    const auto stored_plan = decode_stored_plan(*stored_bytes, expected.resolved_stop_base_plan_id);
    if (*stored_bytes != canonical_bytes || stored_plan != expected) {
      fail_integrity("published_plan_independent_readback_mismatch");
    }
  }

  [[nodiscard]] ResolvedStopBasePlanDTO Load(
      const ResolvedStopBasePlanId& resolved_stop_base_plan_id) const {
    require_owner_process();
    const std::filesystem::path path = path_for(resolved_stop_base_plan_id);
    std::lock_guard process_lock(mutex_);
    const auto file_lock = lock_->acquire(LOCK_SH);
    const auto bytes = read_exact_regular_file(path, maximum_single_plan_bytes_, true);
    if (!bytes.has_value()) {
      fail("missing_reference", "resolved Stop base plan is not present",
           {{"reference_type", "resolved_stop_base_plan"},
            {"reference_id", resolved_stop_base_plan_id.value()},
            {"reason", "resolved_stop_base_plan_missing"}});
    }
    auto plan = decode_stored_plan(*bytes, resolved_stop_base_plan_id);
    // Adopt a final-only crash residue left after link/unlink but before the
    // writer's directory barrier. This remains outside the Stop hot path.
    sync_directory(data_directory_);
    return plan;
  }

 private:
  [[nodiscard]] std::filesystem::path path_for(
      const ResolvedStopBasePlanId& resolved_stop_base_plan_id) const {
    return data_directory_ / (digest_hex(resolved_stop_base_plan_id) + std::string(kFinalSuffix));
  }

  void refresh_index_locked(bool remove_stale_temporaries) {
    std::size_t refreshed_count = 0U;
    std::uint64_t refreshed_bytes = 0U;
    std::size_t stale_count = 0U;
    std::uint64_t stale_bytes = 0U;
    std::vector<std::filesystem::path> stale_temporaries;
    const std::size_t maximum_stale_count = maximum_plan_count_ > kMaximumStaleTemporaryCount / 4U
                                                ? kMaximumStaleTemporaryCount
                                                : maximum_plan_count_ * 4U;

    std::error_code iteration_error;
    for (std::filesystem::directory_iterator
             iterator(data_directory_, std::filesystem::directory_options::none, iteration_error),
         end;
         iterator != end; iterator.increment(iteration_error)) {
      if (iteration_error) {
        fail("resolved_stop_base_plan_repository_storage_failure",
             "cannot enumerate resolved Stop base-plan repository",
             {{"reason", "repository_enumeration_failed"}});
      }
      const std::string filename = iterator->path().filename().string();
      const auto status = iterator->symlink_status(iteration_error);
      if (iteration_error || std::filesystem::is_symlink(status) ||
          !std::filesystem::is_regular_file(status)) {
        fail_integrity("unsafe_repository_entry");
      }
      if (is_stale_temporary_filename(filename)) {
        const auto byte_size = iterator->file_size(iteration_error);
        if (iteration_error || byte_size > maximum_single_plan_bytes_ ||
            ++stale_count > maximum_stale_count ||
            static_cast<std::uint64_t>(byte_size) > maximum_total_bytes_ - stale_bytes) {
          fail("resolved_stop_base_plan_repository_capacity_exhausted",
               "resolved Stop base-plan stale temporary bound is exhausted",
               {{"reason", "stale_temporary_bound_exhausted"}});
        }
        stale_bytes += static_cast<std::uint64_t>(byte_size);
        stale_temporaries.push_back(iterator->path());
        continue;
      }

      const auto expected_id = id_from_filename(filename);
      const auto bytes =
          read_exact_regular_file(iterator->path(), maximum_single_plan_bytes_, false);
      if (!bytes.has_value()) {
        fail_integrity("enumerated_plan_disappeared");
      }
      static_cast<void>(decode_stored_plan(*bytes, expected_id));
      const auto byte_size = static_cast<std::uint64_t>(bytes->size());
      if (refreshed_count >= maximum_plan_count_ ||
          byte_size > maximum_total_bytes_ - refreshed_bytes) {
        fail("resolved_stop_base_plan_repository_capacity_exhausted",
             "persistent resolved Stop base-plan repository exceeds its bounds",
             {{"reason", "persistent_repository_bound_exhausted"}});
      }
      ++refreshed_count;
      refreshed_bytes += byte_size;
    }
    if (iteration_error) {
      fail("resolved_stop_base_plan_repository_storage_failure",
           "cannot enumerate resolved Stop base-plan repository",
           {{"reason", "repository_enumeration_failed"}});
    }

    if (remove_stale_temporaries) {
      for (const auto& temporary : stale_temporaries) {
        if (::unlink(temporary.c_str()) != 0) {
          fail_storage("remove stale resolved Stop base-plan temporary entry");
        }
      }
    }
    sync_directory(data_directory_);
    plan_count_ = refreshed_count;
    retained_bytes_ = refreshed_bytes;
  }

  std::filesystem::path root_;
  std::filesystem::path repository_directory_;
  std::filesystem::path data_directory_;
  std::size_t maximum_plan_count_;
  std::uint64_t maximum_total_bytes_;
  std::uint64_t maximum_single_plan_bytes_;
  std::unique_ptr<ProcessAffineLockFile> lock_;
  std::size_t plan_count_{0U};
  std::uint64_t retained_bytes_{0U};
  mutable std::mutex mutex_;
};

FilesystemResolvedStopBasePlanRepository::FilesystemResolvedStopBasePlanRepository(
    std::filesystem::path trusted_root, std::size_t maximum_plan_count,
    std::uint64_t maximum_total_bytes, std::uint64_t maximum_single_plan_bytes)
    : storage_(std::make_unique<Storage>(std::move(trusted_root), maximum_plan_count,
                                         maximum_total_bytes, maximum_single_plan_bytes)) {}

FilesystemResolvedStopBasePlanRepository::~FilesystemResolvedStopBasePlanRepository() = default;

Result<ResolvedStopBasePlanId> FilesystemResolvedStopBasePlanRepository::PutIfAbsent(
    ResolvedStopBasePlanDTO plan) {
  return as_repository_result<ResolvedStopBasePlanId>([&]() {
    storage_->require_owner_process();
    auto made = MakeResolvedStopBasePlan(std::move(plan));
    if (!made.ok()) {
      throw RepositoryFailure(made.error());
    }
    auto encoded = EncodeResolvedStopBasePlan(made.value());
    if (!encoded.ok()) {
      throw RepositoryFailure(encoded.error());
    }
    storage_->Put(made.value(), encoded.value());
    return made.value().resolved_stop_base_plan_id;
  });
}

Result<ResolvedStopBasePlanDTO> FilesystemResolvedStopBasePlanRepository::LoadVerified(
    const ResolvedStopBasePlanId& resolved_stop_base_plan_id) const {
  return as_repository_result<ResolvedStopBasePlanDTO>(
      [&]() { return storage_->Load(resolved_stop_base_plan_id); });
}

}  // namespace manyforge::core
