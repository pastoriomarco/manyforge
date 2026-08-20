#include "posix_durability_internal.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <charconv>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>
#include <limits>
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

std::optional<std::string> read_exact_regular_file(
    const std::filesystem::path& path, std::uint64_t maximum_bytes,
    bool missing_is_empty, const ErrorPolicy& errors,
    const ReadMessages& messages) {
  const int descriptor =
      ::open(path.c_str(),
             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
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

}  // namespace manyforge::deployment::detail::posix_durability
