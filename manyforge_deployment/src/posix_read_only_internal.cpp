#include "posix_durability_internal.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <charconv>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>

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

[[nodiscard]] std::optional<int>
descriptor_minted_root(const std::filesystem::path &root) noexcept {
  const auto text = root.generic_string();
  constexpr std::string_view prefix = "/proc/self/fd/";
  if (!text.starts_with(prefix) || text.size() == prefix.size()) {
    return std::nullopt;
  }
  if (text[prefix.size()] == '0' && text.size() != prefix.size() + 1U) {
    return std::nullopt;
  }
  unsigned int value = 0U;
  const auto parsed = std::from_chars(text.data() + prefix.size(),
                                      text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value == 0U || value > static_cast<unsigned int>(INT_MAX)) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

}  // namespace

std::filesystem::path normalize_existing_plain_root(
    const std::filesystem::path& root, const ErrorPolicy& errors,
    const TrustedRootMessages& messages) {
  if (root.empty()) {
    fail_semantic(errors, messages.empty);
  }
  if (const auto descriptor = descriptor_minted_root(root);
      descriptor.has_value()) {
    struct stat status {};
    if (::fstat(*descriptor, &status) != 0 || !S_ISDIR(status.st_mode)) {
      fail_semantic(errors, messages.not_existing_plain_directory);
    }
    // This exact procfd spelling is minted only from an owned, retained
    // descriptor. Never canonicalize it back to the mutable operator path.
    return root;
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

void require_existing_plain_directory(
    const std::filesystem::path& directory, const ErrorPolicy& errors,
    const ExistingDirectoryMessages& messages) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(directory, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::filesystem::filesystem_error(
        std::string(messages.inspect), directory, error);
  }
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    fail_semantic(errors, messages.not_existing_plain_directory);
  }
}

namespace {

/// Owns one descriptor from the moment open() returns.
class OwnedDescriptor final {
public:
  explicit OwnedDescriptor(const int value) noexcept : value_(value) {}
  ~OwnedDescriptor() {
    if (value_ >= 0) {
      (void)::close(value_);
    }
  }
  OwnedDescriptor(const OwnedDescriptor&) = delete;
  OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;

  [[nodiscard]] int get() const noexcept { return value_; }

private:
  int value_;
};

[[nodiscard]] RegularFileIdentity identity_of(const struct stat& status) {
  return RegularFileIdentity{
      static_cast<std::uint64_t>(status.st_dev),
      static_cast<std::uint64_t>(status.st_ino),
      static_cast<std::uint64_t>(status.st_size),
      static_cast<std::int64_t>(status.st_mtim.tv_sec),
      static_cast<std::int64_t>(status.st_mtim.tv_nsec),
      static_cast<std::int64_t>(status.st_ctim.tv_sec),
      static_cast<std::int64_t>(status.st_ctim.tv_nsec),
  };
}

/// One bounded regular file opened without following links.
///
/// The identity is observed once, at open, and is the only size this reader
/// ever uses: it has been checked against the byte bound, so no later fstat
/// can enlarge an allocation. A file that changes afterwards fails the exact
/// read (short read, extra byte, or a differing identity after the read).
class BoundedRegularFile final {
public:
  BoundedRegularFile(const std::filesystem::path& path,
                     const std::uint64_t maximum_bytes,
                     const bool missing_is_empty, const ErrorPolicy& errors,
                     const ReadMessages& messages)
      : errors_(errors),
        messages_(messages),
        descriptor_(::open(path.c_str(),
                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)) {
    if (descriptor_.get() < 0) {
      if (missing_is_empty && errno == ENOENT) {
        return;
      }
      fail_errno(messages.open);
    }
    const auto status = Status();
    if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > maximum_bytes ||
        static_cast<std::uint64_t>(status.st_size) >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
      fail_semantic(errors, messages.immutable_bound);
    }
    opened_ = identity_of(status);
  }
  BoundedRegularFile(const BoundedRegularFile&) = delete;
  BoundedRegularFile& operator=(const BoundedRegularFile&) = delete;

  /// Absent only for a permitted missing file.
  [[nodiscard]] const std::optional<RegularFileIdentity>& opened()
      const noexcept {
    return opened_;
  }

  /// Reads exactly the bounded size observed at open.
  [[nodiscard]] std::string ReadExact() const {
    std::string bytes(static_cast<std::size_t>(opened_->size), '\0');
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
      const ssize_t read_count = ::read(descriptor_.get(), bytes.data() + offset,
                                        bytes.size() - offset);
      if (read_count < 0 && errno == EINTR) {
        continue;
      }
      if (read_count <= 0) {
        fail_semantic(errors_, messages_.changed);
      }
      offset += static_cast<std::size_t>(read_count);
    }
    char extra = '\0';
    ssize_t extra_count = 0;
    do {
      extra_count = ::read(descriptor_.get(), &extra, 1U);
    } while (extra_count < 0 && errno == EINTR);
    if (extra_count != 0) {
      fail_semantic(errors_, messages_.grew);
    }
    return bytes;
  }

  /// Fails unless the descriptor still has its identity from open.
  void RequireUnchanged() const {
    if (identity_of(Status()) != *opened_) {
      fail_semantic(errors_, messages_.changed);
    }
  }

private:
  [[nodiscard]] struct stat Status() const {
    struct stat status {};
    if (::fstat(descriptor_.get(), &status) != 0) {
      fail_errno(messages_.stat);
    }
    return status;
  }

  const ErrorPolicy& errors_;
  const ReadMessages& messages_;
  OwnedDescriptor descriptor_;
  std::optional<RegularFileIdentity> opened_;
};

}  // namespace

std::int64_t realtime_now_ns() {
  timespec now{};
  if (::clock_gettime(CLOCK_REALTIME, &now) != 0) {
    fail_errno("read realtime clock");
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
}

bool identity_settled_before(const RegularFileIdentity& identity,
                             const std::int64_t reference_realtime_ns) {
  // Covers coarse kernel clocks and the coarsest supported filesystem
  // timestamp resolution (two seconds).
  constexpr std::int64_t kTimestampGranularityBoundNs = 2'000'000'000LL;
  constexpr std::int64_t kMaximumSeconds =
      std::numeric_limits<std::int64_t>::max() / 1'000'000'000LL - 1;
  if (identity.changed_seconds < 0 ||
      identity.changed_seconds > kMaximumSeconds) {
    return false;
  }
  const std::int64_t changed_ns =
      identity.changed_seconds * 1'000'000'000LL +
      identity.changed_nanoseconds;
  return changed_ns < reference_realtime_ns - kTimestampGranularityBoundNs;
}

std::optional<IdentifiedFileBytes> read_exact_regular_file_with_identity(
    const std::filesystem::path& path, std::uint64_t maximum_bytes,
    bool missing_is_empty, const ErrorPolicy& errors,
    const ReadMessages& messages) {
  const BoundedRegularFile file(path, maximum_bytes, missing_is_empty, errors,
                                messages);
  if (!file.opened().has_value()) {
    return std::nullopt;
  }
  auto bytes = file.ReadExact();
  file.RequireUnchanged();
  return IdentifiedFileBytes{std::move(bytes), *file.opened()};
}

std::optional<std::string> read_exact_regular_file(
    const std::filesystem::path& path, std::uint64_t maximum_bytes,
    bool missing_is_empty, const ErrorPolicy& errors,
    const ReadMessages& messages) {
  const BoundedRegularFile file(path, maximum_bytes, missing_is_empty, errors,
                                messages);
  if (!file.opened().has_value()) {
    return std::nullopt;
  }
  return file.ReadExact();
}

std::optional<RegularFileIdentity> regular_file_identity(
    const std::filesystem::path& path, std::uint64_t maximum_bytes,
    bool missing_is_empty, const ErrorPolicy& errors,
    const ReadMessages& messages) {
  return BoundedRegularFile(path, maximum_bytes, missing_is_empty, errors,
                            messages)
      .opened();
}

}  // namespace manyforge::deployment::detail::posix_durability
