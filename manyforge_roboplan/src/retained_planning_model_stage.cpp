#include "manyforge_roboplan/retained_planning_model_stage.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

constexpr std::string_view kAssetPrefix = "asset://";
constexpr std::string_view kPackagePrefix = "package://";
constexpr std::string_view kStoreDigestPrefix = "sha256:";
constexpr std::string_view kStoreKeyPrefix = "sha256/";
constexpr std::string_view kReadyMarker =
    "manyforge.retained_planning_stage.v1\n";
constexpr std::size_t kMaximumLogicalUriBytes = 2048U;
constexpr std::size_t kMaximumPathSegmentBytes = 128U;

[[noreturn]] void fail(std::string_view message) {
  throw RetainedPlanningStageError(std::string(message));
}

[[noreturn]] void fail_errno(std::string_view operation) {
  throw RetainedPlanningStageError(
      std::string(operation) + ": " + std::strerror(errno));
}

class FileDescriptor final {
 public:
  FileDescriptor() = default;
  explicit FileDescriptor(int value) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      (void)::close(value_);
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        (void)::close(value_);
      }
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return value_; }

 private:
  int value_{-1};
};

struct ParsedAssetUri final {
  std::string logical_uri;
  std::string package_uri;
  std::vector<std::string> path_segments;
};

struct PreparedResource final {
  RetainedPlanningResourceInput input;
  ParsedAssetUri uri;
};

struct OwnedRoot final {
  std::filesystem::path path;
  FileDescriptor descriptor;
};

[[nodiscard]] bool is_ascii_alphanumeric(char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9');
}

[[nodiscard]] bool is_safe_segment_character(char value) noexcept {
  return is_ascii_alphanumeric(value) || value == '.' || value == '_' ||
         value == '-';
}

void require_safe_segment(std::string_view segment) {
  if (segment.empty() || segment.size() > kMaximumPathSegmentBytes ||
      !is_ascii_alphanumeric(segment.front()) ||
      !std::all_of(segment.begin(), segment.end(),
                   is_safe_segment_character)) {
    fail("retained planning resource URI contains an unsafe path segment");
  }
}

[[nodiscard]] ParsedAssetUri parse_asset_uri(std::string logical_uri) {
  if (logical_uri.size() > kMaximumLogicalUriBytes ||
      !logical_uri.starts_with(kAssetPrefix)) {
    fail("retained planning resource URI is not an exact bounded asset URI");
  }
  const std::string_view relative(logical_uri.data() + kAssetPrefix.size(),
                                  logical_uri.size() - kAssetPrefix.size());
  std::vector<std::string> segments;
  std::size_t offset = 0U;
  while (offset <= relative.size()) {
    const std::size_t separator = relative.find('/', offset);
    const std::size_t length =
        separator == std::string_view::npos
            ? relative.size() - offset
            : separator - offset;
    const std::string_view segment = relative.substr(offset, length);
    require_safe_segment(segment);
    segments.emplace_back(segment);
    if (separator == std::string_view::npos) {
      break;
    }
    offset = separator + 1U;
  }
  if (segments.size() < 2U) {
    fail("retained planning resource URI must contain an asset ID and path");
  }
  return {
      logical_uri,
      std::string(kPackagePrefix) + std::string(relative),
      std::move(segments),
  };
}

[[nodiscard]] bool is_lower_hex(char value) noexcept {
  return (value >= '0' && value <= '9') ||
         (value >= 'a' && value <= 'f');
}

[[nodiscard]] std::string sha256_digest(std::string_view bytes) {
  using Context =
      std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  Context context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (context == nullptr ||
      EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
    fail("could not initialize retained planning resource digest");
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0U;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 ||
      digest_size != 32U) {
    fail("could not finalize retained planning resource digest");
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(kStoreDigestPrefix);
  result.reserve(kStoreDigestPrefix.size() + 64U);
  for (std::size_t index = 0U; index < digest_size; ++index) {
    result.push_back(kHex[(digest[index] >> 4U) & 0x0fU]);
    result.push_back(kHex[digest[index] & 0x0fU]);
  }
  return result;
}

void require_digest_and_store_key(
    const RetainedPlanningResourceInput& resource) {
  if (resource.content_digest.size() !=
          kStoreDigestPrefix.size() + 64U ||
      !resource.content_digest.starts_with(kStoreDigestPrefix) ||
      !std::all_of(
          resource.content_digest.begin() +
              static_cast<std::ptrdiff_t>(kStoreDigestPrefix.size()),
          resource.content_digest.end(), is_lower_hex)) {
    fail("retained planning resource has an invalid content digest");
  }
  const std::string expected_key =
      std::string(kStoreKeyPrefix) +
      resource.content_digest.substr(kStoreDigestPrefix.size());
  if (resource.content_addressed_store_key != expected_key) {
    fail("retained planning resource store key does not match its digest");
  }
  if (sha256_digest(resource.bytes) != resource.content_digest) {
    fail("retained planning resource bytes do not match their digest");
  }
}

void require_media_type(std::string_view media_type) {
  static const std::set<std::string_view> kAllowedMediaTypes{
      "image/jpeg",
      "image/png",
      "model/stl",
      "model/vnd.collada+xml",
  };
  if (!kAllowedMediaTypes.contains(media_type)) {
    fail("retained planning resource has an unsupported media type");
  }
}

void checked_add(std::uint64_t& total, std::uint64_t value,
                 std::uint64_t limit, std::string_view context) {
  if (value > limit || total > limit - value) {
    fail(std::string(context) + " exceeds its staging byte bound");
  }
  total += value;
}

void require_limits(const RetainedPlanningStageLimits& limits) {
  if (limits.maximum_resource_count == 0U ||
      limits.maximum_resource_count >
          RetainedPlanningModelStage::kHardMaximumResourceCount ||
      limits.maximum_xml_bytes == 0U ||
      limits.maximum_xml_bytes >
          RetainedPlanningModelStage::kHardMaximumXmlBytes ||
      limits.maximum_resource_bytes == 0U ||
      limits.maximum_resource_bytes >
          RetainedPlanningModelStage::kHardMaximumResourceBytes ||
      limits.maximum_total_bytes == 0U ||
      limits.maximum_total_bytes >
          RetainedPlanningModelStage::kHardMaximumTotalBytes) {
    fail("retained planning staging limits exceed the hard contract");
  }
}

[[nodiscard]] bool path_is_prefix(
    const std::vector<std::string>& left,
    const std::vector<std::string>& right) {
  return left.size() < right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

[[nodiscard]] std::vector<PreparedResource> prepare_resources(
    RetainedPlanningModelInput& input,
    const RetainedPlanningStageLimits& limits) {
  if (input.resources.size() > limits.maximum_resource_count) {
    fail("retained planning model exceeds the resource-count bound");
  }

  std::uint64_t total = 0U;
  if (input.urdf_bytes.empty() || input.srdf_bytes.empty() ||
      input.urdf_bytes.size() > limits.maximum_xml_bytes ||
      input.srdf_bytes.size() > limits.maximum_xml_bytes) {
    fail("retained planning URDF/SRDF violates the XML byte bound");
  }
  checked_add(total, static_cast<std::uint64_t>(input.urdf_bytes.size()),
              limits.maximum_total_bytes, "retained planning model");
  checked_add(total, static_cast<std::uint64_t>(input.srdf_bytes.size()),
              limits.maximum_total_bytes, "retained planning model");

  std::vector<PreparedResource> prepared;
  prepared.reserve(input.resources.size());
  for (auto& resource : input.resources) {
    if (resource.byte_size == 0U ||
        resource.byte_size !=
            static_cast<std::uint64_t>(resource.bytes.size()) ||
        resource.byte_size > limits.maximum_resource_bytes) {
      fail("retained planning resource has an invalid byte size");
    }
    checked_add(total, resource.byte_size, limits.maximum_total_bytes,
                "retained planning model");
    ParsedAssetUri parsed_uri = parse_asset_uri(resource.logical_uri);
    require_media_type(resource.media_type);
    require_digest_and_store_key(resource);
    prepared.push_back({std::move(resource), std::move(parsed_uri)});
  }

  std::sort(
      prepared.begin(), prepared.end(),
      [](const PreparedResource& left, const PreparedResource& right) {
        return left.uri.path_segments < right.uri.path_segments;
      });
  for (std::size_t index = 1U; index < prepared.size(); ++index) {
    const auto& previous = prepared[index - 1U].uri;
    const auto& current = prepared[index].uri;
    if (previous.path_segments == current.path_segments) {
      fail("retained planning model repeats one logical resource URI");
    }
    if (path_is_prefix(previous.path_segments, current.path_segments)) {
      fail("retained planning resource paths contain a file/directory "
           "collision");
    }
  }
  return prepared;
}

[[nodiscard]] std::string derive_loader_urdf(
    std::string_view retained_urdf,
    const std::vector<PreparedResource>& resources) {
  std::map<std::string_view, std::string_view> replacements;
  for (const auto& resource : resources) {
    replacements.emplace(resource.uri.logical_uri,
                         resource.uri.package_uri);
  }

  constexpr std::string_view kFilenamePrefix = "filename=\"";
  std::string output;
  output.reserve(retained_urdf.size());
  std::size_t cursor = 0U;
  while (cursor < retained_urdf.size()) {
    const std::size_t attribute =
        retained_urdf.find(kFilenamePrefix, cursor);
    if (attribute == std::string_view::npos) {
      output.append(retained_urdf.substr(cursor));
      break;
    }
    const std::size_t value_start = attribute + kFilenamePrefix.size();
    const std::size_t value_end = retained_urdf.find('"', value_start);
    if (value_end == std::string_view::npos) {
      fail("retained planning URDF has an unterminated filename attribute");
    }
    output.append(retained_urdf.substr(cursor, value_start - cursor));
    const std::string_view logical_uri =
        retained_urdf.substr(value_start, value_end - value_start);
    const auto replacement = replacements.find(logical_uri);
    if (replacement == replacements.end()) {
      fail("retained planning URDF references an unverified resource");
    }
    output.append(replacement->second);
    output.push_back('"');
    cursor = value_end + 1U;
  }

  if (output.find(kAssetPrefix) != std::string::npos ||
      output.find("file://") != std::string::npos ||
      output.find("model://") != std::string::npos) {
    fail("derived RoboPlan URDF retained a non-package resource lookup");
  }
  return output;
}

[[nodiscard]] std::filesystem::path require_staging_parent(
    const std::filesystem::path& staging_parent) {
  if (staging_parent.empty()) {
    fail("retained planning staging parent must not be empty");
  }
  std::error_code error;
  std::filesystem::path absolute =
      std::filesystem::absolute(staging_parent, error).lexically_normal();
  if (error) {
    throw std::filesystem::filesystem_error(
        "cannot resolve retained planning staging parent", staging_parent,
        error);
  }
  const auto status = std::filesystem::symlink_status(absolute, error);
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    fail("retained planning staging parent is not a plain directory");
  }
  return absolute;
}

[[nodiscard]] std::string random_root_name() {
  std::array<unsigned char, 16U> random_bytes{};
  std::size_t offset = 0U;
  while (offset < random_bytes.size()) {
    const ssize_t count =
        ::getrandom(random_bytes.data() + offset,
                    random_bytes.size() - offset, 0U);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      fail_errno("cannot obtain retained planning staging entropy");
    }
    offset += static_cast<std::size_t>(count);
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string name = "manyforge-retained-planning-";
  name.reserve(name.size() + random_bytes.size() * 2U);
  for (const unsigned char byte : random_bytes) {
    name.push_back(kHex[(byte >> 4U) & 0x0fU]);
    name.push_back(kHex[byte & 0x0fU]);
  }
  return name;
}

[[nodiscard]] OwnedRoot create_private_root(
    const std::filesystem::path& staging_parent) {
  const std::filesystem::path parent =
      require_staging_parent(staging_parent);
  FileDescriptor parent_descriptor(
      ::open(parent.c_str(),
             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (parent_descriptor.get() < 0) {
    fail_errno("cannot open retained planning staging parent");
  }

  for (std::size_t attempt = 0U; attempt < 32U; ++attempt) {
    const std::string name = random_root_name();
    if (::mkdirat(parent_descriptor.get(), name.c_str(), 0700) == 0) {
      FileDescriptor root_descriptor(
          ::openat(parent_descriptor.get(), name.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (root_descriptor.get() < 0) {
        const int saved_errno = errno;
        (void)::unlinkat(
            parent_descriptor.get(), name.c_str(), AT_REMOVEDIR);
        errno = saved_errno;
        fail_errno("cannot open private retained planning staging root");
      }
      struct stat status {};
      if (::fstat(root_descriptor.get(), &status) != 0) {
        const int saved_errno = errno;
        (void)::unlinkat(
            parent_descriptor.get(), name.c_str(), AT_REMOVEDIR);
        errno = saved_errno;
        fail_errno("cannot inspect private retained planning staging root");
      }
      if (!S_ISDIR(status.st_mode) || (status.st_mode & 0777) != 0700) {
        (void)::unlinkat(
            parent_descriptor.get(), name.c_str(), AT_REMOVEDIR);
        fail("private retained planning staging root has unsafe permissions");
      }
      return {parent / name, std::move(root_descriptor)};
    }
    if (errno != EEXIST) {
      fail_errno("cannot create private retained planning staging root");
    }
  }
  fail("could not allocate a unique retained planning staging root");
}

[[nodiscard]] FileDescriptor duplicate_descriptor(int descriptor) {
  const int duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
  if (duplicate < 0) {
    fail_errno("cannot duplicate retained planning directory descriptor");
  }
  return FileDescriptor(duplicate);
}

[[nodiscard]] FileDescriptor open_or_create_directory(
    int parent_descriptor, std::string_view name) {
  const std::string owned_name(name);
  if (::mkdirat(parent_descriptor, owned_name.c_str(), 0700) != 0 &&
      errno != EEXIST) {
    fail_errno("cannot create retained planning resource directory");
  }
  FileDescriptor descriptor(
      ::openat(parent_descriptor, owned_name.c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (descriptor.get() < 0) {
    fail_errno("cannot open retained planning resource directory");
  }
  struct stat status {};
  if (::fstat(descriptor.get(), &status) != 0) {
    fail_errno("cannot inspect retained planning resource directory");
  }
  if (!S_ISDIR(status.st_mode) || (status.st_mode & 0077) != 0) {
    fail("retained planning resource directory is not private");
  }
  return descriptor;
}

void write_all(int descriptor, std::string_view bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const std::size_t chunk = std::min<std::size_t>(
        remaining,
        static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t written =
        ::write(descriptor, bytes.data() + offset, chunk);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      fail_errno("cannot write retained planning resource");
    }
    offset += static_cast<std::size_t>(written);
  }
}

[[nodiscard]] std::string read_exact(int directory_descriptor,
                                     std::string_view filename,
                                     std::uint64_t expected_size) {
  const std::string owned_filename(filename);
  FileDescriptor descriptor(
      ::openat(directory_descriptor, owned_filename.c_str(),
               O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (descriptor.get() < 0) {
    fail_errno("cannot reopen retained planning resource");
  }
  struct stat status {};
  if (::fstat(descriptor.get(), &status) != 0) {
    fail_errno("cannot inspect retained planning resource");
  }
  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) != expected_size ||
      status.st_nlink != 1) {
    fail("retained planning resource read-back metadata changed");
  }
  std::string result(static_cast<std::size_t>(expected_size), '\0');
  std::size_t offset = 0U;
  while (offset < result.size()) {
    const ssize_t count =
        ::read(descriptor.get(), result.data() + offset,
               result.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      fail("retained planning resource changed during read-back");
    }
    offset += static_cast<std::size_t>(count);
  }
  char extra = '\0';
  ssize_t extra_count = 0;
  do {
    extra_count = ::read(descriptor.get(), &extra, 1U);
  } while (extra_count < 0 && errno == EINTR);
  if (extra_count != 0) {
    fail("retained planning resource grew during read-back");
  }
  return result;
}

void publish_file(
    int root_descriptor, const std::vector<std::string>& path_segments,
    std::string_view bytes, std::string_view expected_digest,
    std::size_t publication_index) {
  if (path_segments.empty()) {
    throw std::logic_error("retained planning publication path is empty");
  }
  FileDescriptor directory = duplicate_descriptor(root_descriptor);
  for (std::size_t index = 0U; index + 1U < path_segments.size(); ++index) {
    directory =
        open_or_create_directory(directory.get(), path_segments[index]);
  }

  const std::string& filename = path_segments.back();
  const std::string temporary =
      ".manyforge-partial-" + std::to_string(publication_index);
  FileDescriptor output(
      ::openat(directory.get(), temporary.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (output.get() < 0) {
    fail_errno("cannot create retained planning temporary resource");
  }
  try {
    write_all(output.get(), bytes);
    if (::fsync(output.get()) != 0) {
      fail_errno("cannot synchronize retained planning resource");
    }
    if (::fchmod(output.get(), 0400) != 0) {
      fail_errno("cannot make retained planning resource read-only");
    }
    if (::fsync(output.get()) != 0) {
      fail_errno("cannot synchronize retained planning resource metadata");
    }
    if (::linkat(directory.get(), temporary.c_str(), directory.get(),
                 filename.c_str(), 0) != 0) {
      fail_errno("cannot exclusively publish retained planning resource");
    }
    if (::unlinkat(directory.get(), temporary.c_str(), 0) != 0) {
      fail_errno("cannot remove retained planning temporary resource");
    }
    if (::fsync(directory.get()) != 0) {
      fail_errno("cannot synchronize retained planning resource directory");
    }
  } catch (...) {
    (void)::unlinkat(directory.get(), temporary.c_str(), 0);
    throw;
  }

  const std::string read_back = read_exact(
      directory.get(), filename, static_cast<std::uint64_t>(bytes.size()));
  if (read_back != bytes ||
      (!expected_digest.empty() &&
       sha256_digest(read_back) != expected_digest)) {
    fail("retained planning resource failed exact publication read-back");
  }
}

void publish_ready_marker(int root_descriptor,
                          std::size_t publication_index) {
  publish_file(
      root_descriptor, {".manyforge-ready"}, kReadyMarker,
      sha256_digest(kReadyMarker), publication_index);
  if (::fsync(root_descriptor) != 0) {
    fail_errno("cannot synchronize retained planning staging root");
  }
}

}  // namespace

RetainedPlanningModelStage RetainedPlanningModelStage::create(
    RetainedPlanningModelInput input,
    const std::filesystem::path& staging_parent,
    RetainedPlanningStageLimits limits) {
  require_limits(limits);
  std::vector<PreparedResource> resources =
      prepare_resources(input, limits);
  std::string loader_urdf =
      derive_loader_urdf(input.urdf_bytes, resources);
  if (input.srdf_bytes.find(kAssetPrefix) != std::string::npos ||
      input.srdf_bytes.find(kPackagePrefix) != std::string::npos ||
      input.srdf_bytes.find("file://") != std::string::npos ||
      input.srdf_bytes.find("model://") != std::string::npos) {
    fail("retained planning SRDF contains a resource lookup");
  }

  OwnedRoot root = create_private_root(staging_parent);
  struct FailureCleanup final {
    std::filesystem::path path;
    bool active{true};
    ~FailureCleanup() {
      if (active) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    }
  } cleanup{root.path};

  std::size_t publication_index = 0U;
  for (const auto& resource : resources) {
    publish_file(root.descriptor.get(), resource.uri.path_segments,
                 resource.input.bytes, resource.input.content_digest,
                 publication_index++);
  }
  publish_file(root.descriptor.get(), {"retained-model.urdf"},
               loader_urdf, sha256_digest(loader_urdf),
               publication_index++);
  publish_file(root.descriptor.get(), {"retained-model.srdf"},
               input.srdf_bytes, sha256_digest(input.srdf_bytes),
               publication_index++);
  publish_ready_marker(root.descriptor.get(), publication_index);

  cleanup.active = false;
  return RetainedPlanningModelStage(
      std::move(root.path), std::move(loader_urdf),
      std::move(input.srdf_bytes));
}

RetainedPlanningModelStage::RetainedPlanningModelStage(
    std::filesystem::path staging_root, std::string loader_urdf_bytes,
    std::string srdf_bytes)
    : staging_root_(std::move(staging_root)),
      loader_urdf_bytes_(std::move(loader_urdf_bytes)),
      srdf_bytes_(std::move(srdf_bytes)),
      package_paths_{staging_root_} {}

RetainedPlanningModelStage::~RetainedPlanningModelStage() {
  cleanup();
}

RetainedPlanningModelStage::RetainedPlanningModelStage(
    RetainedPlanningModelStage&& other) noexcept
    : staging_root_(std::move(other.staging_root_)),
      loader_urdf_bytes_(std::move(other.loader_urdf_bytes_)),
      srdf_bytes_(std::move(other.srdf_bytes_)),
      package_paths_(std::move(other.package_paths_)) {
  other.staging_root_.clear();
  other.package_paths_.clear();
}

RetainedPlanningModelStage& RetainedPlanningModelStage::operator=(
    RetainedPlanningModelStage&& other) noexcept {
  if (this != &other) {
    cleanup();
    staging_root_ = std::move(other.staging_root_);
    loader_urdf_bytes_ = std::move(other.loader_urdf_bytes_);
    srdf_bytes_ = std::move(other.srdf_bytes_);
    package_paths_ = std::move(other.package_paths_);
    other.staging_root_.clear();
    other.package_paths_.clear();
  }
  return *this;
}

void RetainedPlanningModelStage::cleanup() noexcept {
  if (staging_root_.empty()) {
    return;
  }
  std::error_code ignored;
  std::filesystem::remove_all(staging_root_, ignored);
  staging_root_.clear();
  package_paths_.clear();
}

}  // namespace manyforge::roboplan_backend
