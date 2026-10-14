#include "execution_host_posix.hpp"
#include "execution_host_commands.hpp"
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <set>
#include <stdexcept>
#include <system_error>

namespace manyforge::roboplan_backend::host_detail {

FileDescriptor::FileDescriptor(int value) noexcept : value_(value) {}

FileDescriptor::~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }

FileDescriptor::FileDescriptor(FileDescriptor &&other) noexcept
      : value_(std::exchange(other.value_, -1)) {}

FileDescriptor &FileDescriptor::operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        static_cast<void>(::close(value_));
      }
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }

[[nodiscard]] int FileDescriptor::get() const noexcept { return value_; }

[[nodiscard]] FileDescriptor::operator bool() const noexcept { return value_ >= 0; }

HostLock::HostLock(const std::filesystem::path &run_root) {
    const auto path = run_root / std::string(kLockFileName);
    descriptor_ = FileDescriptor(
        ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
               S_IRUSR | S_IWUSR));
    if (!descriptor_) {
      FailErrno("open execution-host lock");
    }
    struct stat status {};
    if (::fstat(descriptor_.get(), &status) != 0) {
      FailErrno("inspect execution-host lock");
    }
    Require(S_ISREG(status.st_mode),
            "execution-host lock is not a regular file");
    if (::flock(descriptor_.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        Fail("another execution host already owns this run root");
      }
      FailErrno("acquire execution-host lock");
    }
  }

CommandSocket::CommandSocket(const std::filesystem::path &run_root,
                std::string_view socket_file_name)
      : path_(run_root / std::string(socket_file_name)) {
    const auto spelling = path_.string();
    Require(spelling.size() < sizeof(sockaddr_un::sun_path),
            "execution command socket path is too long");
    struct stat existing {};
    if (::lstat(path_.c_str(), &existing) == 0) {
      Require(S_ISSOCK(existing.st_mode),
              "execution command path exists and is not a socket");
      if (::unlink(path_.c_str()) != 0) {
        FailErrno("remove stale execution command socket");
      }
    } else if (errno != ENOENT) {
      FailErrno("inspect execution command socket path");
    }
    descriptor_ = FileDescriptor(
        ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!descriptor_) {
      FailErrno("create execution command socket");
    }
    const int send_buffer_bytes = static_cast<int>(kMaximumResponseBytes);
    if (::setsockopt(descriptor_.get(), SOL_SOCKET, SO_SNDBUF,
                     &send_buffer_bytes, sizeof(send_buffer_bytes)) != 0) {
      FailErrno("set execution command response buffer");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, spelling.c_str(), spelling.size() + 1U);
    if (::bind(descriptor_.get(), reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0) {
      FailErrno("bind execution command socket");
    }
    if (::chmod(path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
      FailErrno("restrict execution command socket permissions");
    }
    if (::lstat(path_.c_str(), &bound_status_) != 0 ||
        !S_ISSOCK(bound_status_.st_mode)) {
      Fail("bound execution command path is not a socket");
    }
    if (::listen(descriptor_.get(), kSocketBacklog) != 0) {
      FailErrno("listen on execution command socket");
    }
  }

CommandSocket::~CommandSocket() {
    struct stat current {};
    if (::lstat(path_.c_str(), &current) == 0 &&
        current.st_dev == bound_status_.st_dev &&
        current.st_ino == bound_status_.st_ino) {
      static_cast<void>(::unlink(path_.c_str()));
    }
  }

[[nodiscard]] int CommandSocket::get() const noexcept { return descriptor_.get(); }

volatile sig_atomic_t g_stop_requested = 0;

/// Readable once a stop signal arrived, so socket lanes wait without a
/// timeout. Created before the handlers are installed and never closed: the
/// handler can never write to a closed or reused descriptor.
std::atomic<int> g_stop_signal_wake_fd{-1};
static_assert(std::atomic<int>::is_always_lock_free);

void HandleStopSignal(int) {
  g_stop_requested = 1;
  const int wake = g_stop_signal_wake_fd.load(std::memory_order_relaxed);
  if (wake >= 0) {
    const int saved_errno = errno;
    const std::uint64_t one = 1U;
    const ssize_t written = ::write(wake, &one, sizeof(one));
    // This nonblocking wake is best effort; the stop flag above remains the
    // authoritative signal fact. Preserve the interrupted code's errno.
    static_cast<void>(written);
    errno = saved_errno;
  }
}

[[nodiscard]] std::string SelectedExecutionOfferDigest() {
  constexpr auto family =
      deployment::ros2_control_execution_runtime_contract::kImplementationId;
  const auto &offers = deployment::runtime_contract_declaration_registry()
                           .package_manifest_set()
                           .offers;
  const auto found =
      std::find_if(offers.begin(), offers.end(), [&](const auto &offer) {
        return offer.runtime_implementation_id.value() == family;
      });
  if (found == offers.end()) {
    throw std::runtime_error(
        "selected execution host has no qualified runtime offer");
  }
  return found->offer_digest.value();
}

[[noreturn]] void Fail(std::string detail) {
  throw std::runtime_error("Native execution host: " + std::move(detail));
}

[[noreturn]] void FailErrno(std::string_view context) {
  Fail(std::string(context) + ": " +
       std::system_error(errno, std::generic_category()).what());
}

void Require(bool condition, std::string detail) {
  if (!condition) {
    Fail(std::move(detail));
  }
}

[[nodiscard]] std::filesystem::path
ExistingDirectory(std::filesystem::path path, std::string_view label) {
  Require(!path.empty(), std::string(label) + " cannot be empty");
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  Require(!error && !std::filesystem::is_symlink(status) &&
              std::filesystem::is_directory(status),
          std::string(label) + " must be an existing non-symlink directory");
  auto absolute = std::filesystem::absolute(path, error);
  Require(!error, std::string(label) + " cannot be resolved");
  return absolute.lexically_normal();
}

[[nodiscard]] std::filesystem::path
ExistingPrivateDirectory(std::filesystem::path path,
                         const std::string_view label) {
  Require(path.is_absolute(),
          std::string(label) + " must be an absolute directory capability");
  Require(path.lexically_normal() == path,
          std::string(label) + " must use its canonical lexical spelling");
  std::error_code error;
  Require(std::filesystem::canonical(path, error) == path && !error,
          std::string(label) + " must be one canonical directory capability");
  struct stat before {};
  Require(::lstat(path.c_str(), &before) == 0 && S_ISDIR(before.st_mode) &&
              !S_ISLNK(before.st_mode),
          std::string(label) + " must be one plain directory");
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  struct stat opened {};
  struct stat after {};
  const bool valid =
      descriptor >= 0 && ::fstat(descriptor, &opened) == 0 &&
      ::lstat(path.c_str(), &after) == 0 && opened.st_dev == before.st_dev &&
      opened.st_ino == before.st_ino && after.st_dev == before.st_dev &&
      after.st_ino == before.st_ino && S_ISDIR(opened.st_mode) &&
      S_ISDIR(after.st_mode) && opened.st_uid == ::geteuid() &&
      after.st_uid == ::geteuid() &&
      (opened.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
      (after.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
      (opened.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) ==
          (S_IRUSR | S_IWUSR | S_IXUSR) &&
      (after.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) ==
          (S_IRUSR | S_IWUSR | S_IXUSR);
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
  }
  Require(valid, std::string(label) + " identity, owner, or mode is unsafe");
  return path;
}

void RequireDisjointDirectories(const std::filesystem::path &left,
                                const std::filesystem::path &right,
                                const std::string_view labels) {
  std::error_code error;
  const auto canonical_left = std::filesystem::canonical(left, error);
  Require(!error, std::string(labels) + " left directory is unavailable");
  const auto canonical_right = std::filesystem::canonical(right, error);
  Require(!error, std::string(labels) + " right directory is unavailable");
  const auto mismatch =
      std::mismatch(canonical_left.begin(), canonical_left.end(),
                    canonical_right.begin(), canonical_right.end());
  Require(mismatch.first != canonical_left.end() &&
              mismatch.second != canonical_right.end(),
          std::string(labels) + " must be disjoint directory trees");
}

void RequirePathOutsideDirectory(const std::filesystem::path &path,
                                 const std::filesystem::path &directory,
                                 const std::string_view labels) {
  if (path.empty()) {
    return;
  }
  std::error_code error;
  const auto canonical_parent =
      std::filesystem::canonical(path.parent_path(), error);
  Require(!error, std::string(labels) + " parent is unavailable");
  const auto canonical_path = canonical_parent / path.filename();
  const auto canonical_directory = std::filesystem::canonical(directory, error);
  Require(!error, std::string(labels) + " directory is unavailable");
  const auto mismatch =
      std::mismatch(canonical_directory.begin(), canonical_directory.end(),
                    canonical_path.begin(), canonical_path.end());
  Require(mismatch.first != canonical_directory.end(),
          std::string(labels) + " must be outside disposable run storage");
}

[[nodiscard]] std::filesystem::path
ExistingRegularFile(std::filesystem::path path, std::string_view label,
                    bool executable) {
  Require(!path.empty(), std::string(label) + " cannot be empty");
  struct stat status {};
  Require(::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
              !S_ISLNK(status.st_mode),
          std::string(label) + " must be an existing non-symlink file");
  Require(!executable || ::access(path.c_str(), X_OK) == 0,
          std::string(label) + " must be executable");
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  Require(!error, std::string(label) + " cannot be resolved");
  return absolute.lexically_normal();
}

[[nodiscard]] std::string RandomToken(std::string_view prefix) {
  std::array<unsigned char, kOwnerEpochRandomBytes> bytes{};
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count =
        ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      FailErrno("mint execution-host epoch");
    }
    offset += static_cast<std::size_t>(count);
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(prefix);
  result.reserve(result.size() + bytes.size() * 2U);
  for (const auto byte : bytes) {
    result.push_back(kHex[(byte >> 4U) & 0x0fU]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

[[nodiscard]] component_manager::IsaacSiteSettings
LoadManagedIsaacSiteSettings(const std::filesystem::path &path) {
  FileDescriptor file(::open(path.c_str(),
                             O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
  if (file.get() < 0) {
    FailErrno("open managed Isaac site snapshot " + path.string());
  }
  struct stat status {};
  if (::fstat(file.get(), &status) != 0) {
    FailErrno("inspect managed Isaac site snapshot " + path.string());
  }
  Require(S_ISREG(status.st_mode),
          "managed Isaac site snapshot must be a regular file: " + path.string());
  std::string document;
  std::array<char, 4096U> buffer{};
  while (true) {
    const auto count = ::read(file.get(), buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      FailErrno("read managed Isaac site snapshot " + path.string());
    }
    if (count == 0) {
      break;
    }
    document.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return component_manager::ParseIsaacSiteSettings(document);
}

[[nodiscard]] bool
IsPortableRuntimeIdentity(const std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U ||
      !((value.front() >= 'a' && value.front() <= 'z') ||
        (value.front() >= 'A' && value.front() <= 'Z') ||
        (value.front() >= '0' && value.front() <= '9'))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](const char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '-';
  });
}

[[nodiscard]] int ParseFileDescriptor(const std::string_view value) {
  Require(!value.empty() && (value.size() == 1U || value.front() != '0'),
          "managed runtime channel FD is not canonical");
  unsigned int result = 0U;
  for (const char digit : value) {
    Require(digit >= '0' && digit <= '9',
            "managed runtime channel FD is not decimal");
    const auto next = static_cast<unsigned int>(digit - '0');
    Require(result <= (static_cast<unsigned int>(INT_MAX) - next) / 10U,
            "managed runtime channel FD overflows");
    result = result * 10U + next;
  }
  return static_cast<int>(result);
}

[[nodiscard]] ManagedRuntimeChildConfig
ParseManagedRuntimeArguments(const int argc, char **argv) {
  ManagedRuntimeChildConfig config;
  std::set<std::string> seen;
  for (int index = 1; index < argc; index += 2) {
    Require(index + 1 < argc,
            "every managed runtime option requires one value");
    const std::string option(argv[index]);
    const std::string value(argv[index + 1]);
    Require(seen.insert(option).second,
            "duplicate managed runtime option " + option);
    if (option == "--manyforge-managed-runtime-channel-fd") {
      config.manager_channel_fd = ParseFileDescriptor(value);
    } else if (option == "--manyforge-managed-liveness-channel-fd") {
      config.manager_liveness_channel_fd = ParseFileDescriptor(value);
    } else if (option == "--manyforge-managed-package-root-fd") {
      config.package_root_fd = ParseFileDescriptor(value);
    } else if (option == "--manyforge-managed-program-artifact-root-fd") {
      config.program_artifact_root_fd = ParseFileDescriptor(value);
    } else if (option == "--manyforge-managed-scene-asset-root-fd") {
      config.scene_asset_root_fd = ParseFileDescriptor(value);
    } else if (option == "--manyforge-managed-isaac-site-settings") {
      config.isaac_site_settings = LoadManagedIsaacSiteSettings(value);
    } else if (option == "--manyforge-managed-isaac-pose-observer-executable") {
      Require(!value.empty(), "managed observer executable path is empty");
      config.isaac_pose_observer_executable = value;
    } else if (option == "--manyforge-managed-isaac-observer-snapshot" ||
               option == "--manyforge-managed-isaac-observer-source-id" ||
               option == "--manyforge-managed-isaac-observer-epoch-base") {
      if (!config.isaac_pose_observer_launch_facts)
        config.isaac_pose_observer_launch_facts.emplace();
      auto &facts = *config.isaac_pose_observer_launch_facts;
      if (option == "--manyforge-managed-isaac-observer-snapshot")
        facts.retained_snapshot_path = value;
      else if (option == "--manyforge-managed-isaac-observer-source-id") {
        const auto decoded = deployment::parse_strict_json_model_yaml(value).root();
        const auto *source = std::get_if<std::string>(&decoded.storage());
        Require(source != nullptr, "managed observer source must be a JSON string");
        facts.source_id = *source;
      }
      else
        facts.source_epoch_base = value;
    } else if (option == "--manyforge-managed-manager-state-root") {
      config.manager_state_root = value;
    } else if (option == "--manyforge-managed-manager-run-root") {
      config.manager_run_root = value;
    } else if (option == "--manyforge-managed-logical-scene-database") {
      config.logical_scene_database = value;
    } else if (option == "--manyforge-managed-program-cycle-journal") {
      config.program_cycle_journal = value;
    } else if (option == "--manyforge-managed-execution-state-root") {
      config.execution_state_root = value;
    } else if (option == "--manyforge-managed-execution-run-root") {
      config.execution_run_root = value;
    } else if (option == "--manyforge-managed-candidate-worker") {
      config.candidate_worker = value;
    } else if (option == "--manyforge-managed-cell-id") {
      config.cell_id = value;
    } else if (option == "--manyforge-managed-retained-bundle-id") {
      config.retained_bundle_id = core::RetainedBundleId(value);
    } else if (option == "--manyforge-managed-graph-revision") {
      config.graph_revision = core::GraphRevision(value);
    } else if (option == "--runtime-unit-id") {
      config.runtime_unit_id = value;
    } else if (option == "--runtime-epoch") {
      config.runtime_epoch = value;
    } else if (option == "--product-identity") {
      config.product_identity = value;
    } else if (option == "--configuration-identity") {
      config.configuration_identity = value;
    } else {
      Fail("unknown managed runtime option " + option);
    }
  }
  Require(seen.size() >= 15U && seen.size() <= 24U &&
              config.manager_channel_fd >= 0 &&
              config.manager_liveness_channel_fd >= 0 &&
              config.manager_liveness_channel_fd != config.manager_channel_fd &&
              config.package_root_fd == kManagedPackageRootFd &&
              config.program_artifact_root_fd ==
                  kManagedProgramArtifactRootFd,
          "managed runtime invocation is incomplete");
  Require(IsPortableRuntimeIdentity(config.cell_id) &&
              IsPortableRuntimeIdentity(config.runtime_unit_id) &&
              IsPortableRuntimeIdentity(config.runtime_epoch),
          "managed runtime identity is invalid");
  Require(!config.isaac_pose_observer_launch_facts ||
              (config.isaac_pose_observer_launch_facts->valid() &&
               config.isaac_pose_observer_executable.has_value()),
          "managed observer launch facts are incomplete or invalid");
  Require(!config.retained_bundle_id.empty() && !config.graph_revision.empty(),
          "managed runtime retained descriptor is invalid");
  Require(core::IsExactLowercaseSha256Digest(config.product_identity) &&
              core::IsExactLowercaseSha256Digest(config.configuration_identity),
          "managed runtime retained identity is invalid");
  struct stat package_status {};
  struct stat program_artifact_status {};
  const int package_flags = ::fcntl(config.package_root_fd, F_GETFD);
  const int program_artifact_flags =
      ::fcntl(config.program_artifact_root_fd, F_GETFD);
  Require(::fstat(config.package_root_fd, &package_status) == 0 &&
              S_ISDIR(package_status.st_mode) && package_flags >= 0 &&
              (package_flags & FD_CLOEXEC) == 0 &&
              ::fstat(config.program_artifact_root_fd,
                      &program_artifact_status) == 0 &&
              S_ISDIR(program_artifact_status.st_mode) &&
              program_artifact_flags >= 0 &&
              (program_artifact_flags & FD_CLOEXEC) == 0,
          "managed package-root descriptor is not a retained directory");
  if (config.scene_asset_root_fd.has_value()) {
    struct stat scene_asset_status {};
    const int scene_asset_flags =
        ::fcntl(*config.scene_asset_root_fd, F_GETFD);
    Require(*config.scene_asset_root_fd == kManagedSceneAssetRootFd &&
                ::fstat(*config.scene_asset_root_fd, &scene_asset_status) ==
                    0 &&
                S_ISDIR(scene_asset_status.st_mode) && scene_asset_flags >= 0 &&
                (scene_asset_flags & FD_CLOEXEC) == 0,
            "managed scene-asset root descriptor is not an inherited "
            "directory");
  }
  config.package_root = std::filesystem::path("/proc/self/fd") /
                        std::to_string(config.package_root_fd);
  config.program_artifact_root =
      std::filesystem::path("/proc/self/fd") /
      std::to_string(config.program_artifact_root_fd);
  if (program_artifact_status.st_dev == package_status.st_dev &&
      program_artifact_status.st_ino == package_status.st_ino) {
    config.program_artifact_root.clear();
  }
  config.manager_state_root = ExistingPrivateDirectory(
      std::move(config.manager_state_root), "managed Manager state root");
  config.manager_run_root = ExistingPrivateDirectory(
      std::move(config.manager_run_root), "managed Manager run root");
  if (!config.logical_scene_database.empty()) {
    config.logical_scene_database =
        ExistingRegularFile(std::move(config.logical_scene_database),
                            "managed logical-scene database", false);
  }
  const auto validate_future_database = [](std::filesystem::path path,
                                           const std::string_view label) {
    Require(!path.empty() && path.has_filename() && path.filename() != "." &&
                path.filename() != "..",
            std::string(label) + " path is invalid");
    static_cast<void>(ExistingPrivateDirectory(path.parent_path(),
                                               std::string(label) + " parent"));
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    Require((!error || error == std::errc::no_such_file_or_directory) &&
                !std::filesystem::is_symlink(status),
            std::string(label) + " path is not a safe physical leaf");
    return path;
  };
  if (!config.program_cycle_journal.empty()) {
    config.program_cycle_journal =
        validate_future_database(std::move(config.program_cycle_journal),
                                 "managed Program-cycle journal");
  }
  config.execution_state_root = ExistingPrivateDirectory(
      std::move(config.execution_state_root), "managed execution state root");
  config.execution_run_root = ExistingPrivateDirectory(
      std::move(config.execution_run_root), "managed execution run root");
  const std::array<const std::filesystem::path *, 4U> typed_roots{
      &config.manager_state_root, &config.manager_run_root,
      &config.execution_state_root, &config.execution_run_root};
  for (std::size_t left = 0U; left < typed_roots.size(); ++left) {
    for (std::size_t right = left + 1U; right < typed_roots.size(); ++right) {
      RequireDisjointDirectories(*typed_roots[left], *typed_roots[right],
                                 "managed state/run capabilities");
    }
  }
  for (const auto *typed_root : typed_roots) {
    RequireDisjointDirectories(config.package_root, *typed_root,
                               "managed install/runtime capabilities");
    if (!config.program_artifact_root.empty()) {
      RequireDisjointDirectories(config.program_artifact_root, *typed_root,
                                 "managed Program/runtime capabilities");
    }
  }
  const std::array<const std::filesystem::path *, 2U> durable_paths{
      &config.logical_scene_database, &config.program_cycle_journal};
  const std::array<const std::filesystem::path *, 2U> run_roots{
      &config.manager_run_root, &config.execution_run_root};
  for (const auto *durable_path : durable_paths) {
    for (const auto *run_root : run_roots) {
      RequirePathOutsideDirectory(*durable_path, *run_root,
                                  "managed durable path/run capability");
    }
  }
  config.candidate_worker = ExistingRegularFile(
      std::move(config.candidate_worker), "managed candidate worker", true);

  const auto require_parent_channel = [](const int descriptor,
                                         const std::string_view label) {
    struct stat status {};
    int socket_type = 0;
    socklen_t socket_type_size = sizeof(socket_type);
    ucred credentials{};
    socklen_t credentials_size = sizeof(credentials);
    Require(::fstat(descriptor, &status) == 0 && S_ISSOCK(status.st_mode) &&
                ::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &socket_type,
                             &socket_type_size) == 0 &&
                socket_type_size == sizeof(socket_type) &&
                socket_type == SOCK_SEQPACKET &&
                ::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials,
                             &credentials_size) == 0 &&
                credentials_size == sizeof(credentials) &&
                credentials.uid == ::geteuid() &&
                credentials.pid == ::getppid(),
            std::string(label) + " is not the direct same-user parent channel");
  };
  require_parent_channel(config.manager_channel_fd, "managed runtime channel");
  require_parent_channel(config.manager_liveness_channel_fd,
                         "managed runtime liveness channel");
  return config;
}

void SendManagedRuntimePacket(const int fd, const std::string_view bytes) {
  Require(
      !bytes.empty() &&
          bytes.size() <=
              component_manager::detail::kMaximumManagedRuntimeRpcPacketBytes,
      "managed runtime response is empty or oversized");
  pollfd descriptor{fd, POLLOUT | POLLHUP, 0};
  int ready = -1;
  do {
    ready = ::poll(&descriptor, 1U, 750);
  } while (ready < 0 && errno == EINTR && g_stop_requested == 0);
  Require(ready > 0 && (descriptor.revents & POLLOUT) != 0,
          "managed runtime parent did not accept a response");
  ssize_t sent = -1;
  do {
    sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  Require(sent == static_cast<ssize_t>(bytes.size()),
          "managed runtime response was not sent atomically");
}

[[nodiscard]] std::optional<std::string>
ReceiveManagedRuntimePacket(const int fd) {
  std::array<char,
             component_manager::detail::kMaximumManagedRuntimeRpcPacketBytes>
      buffer{};
  iovec vector{buffer.data(), buffer.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  ssize_t size = -1;
  do {
    size = ::recvmsg(fd, &message, 0);
  } while (size < 0 && errno == EINTR);
  if (size == 0) {
    return std::nullopt;
  }
  Require(size > 0, "managed runtime request is absent");
  if ((message.msg_flags & MSG_TRUNC) != 0) {
    // Consume and reject a foreign oversized packet without terminating the
    // sole managed child. The ordinary malformed-request path returns one
    // bounded failure packet to the caller.
    return std::string("oversized-managed-runtime-request");
  }
  return std::string(buffer.data(), static_cast<std::size_t>(size));
}

[[nodiscard]] CanonicalValue
ExchangeManagerCommand(const std::filesystem::path &manager_run_root,
                       CanonicalValue command,
                       const std::stop_token stop_token) {
  Require(!stop_token.stop_requested(), "Manager scene request cancelled");
  const auto bytes = content_identity::canonical_json(command);
  Require(!bytes.empty() && bytes.size() <= kMaximumPacketBytes,
          "Manager scene request is empty or oversized");
  const auto socket_path =
      manager_run_root / std::string(kManagerSocketFileName);
  const auto spelling = socket_path.string();
  Require(spelling.size() < sizeof(sockaddr_un::sun_path),
          "Manager command socket path is too long");
  struct stat socket_status {};
  Require(::lstat(socket_path.c_str(), &socket_status) == 0 &&
              S_ISSOCK(socket_status.st_mode) &&
              !S_ISLNK(socket_status.st_mode),
          "Manager command path is not a local socket");

  FileDescriptor descriptor(
      ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!descriptor) {
    FailErrno("create Manager scene client socket");
  }
  // Wake an outstanding socket wait when the hosting execution surface drains.
  // The callback is destroyed before the descriptor, so it cannot affect a
  // subsequently reused descriptor. No owner state or Scene revision is cached.
  const std::stop_callback cancel_wait(stop_token, [&descriptor] {
    static_cast<void>(::shutdown(descriptor.get(), SHUT_RDWR));
  });
  const int receive_buffer_bytes =
      static_cast<int>(kMaximumManagerResponseBytes);
  const int send_buffer_bytes = static_cast<int>(kMaximumPacketBytes);
  if (::setsockopt(descriptor.get(), SOL_SOCKET, SO_RCVBUF,
                   &receive_buffer_bytes, sizeof(receive_buffer_bytes)) != 0 ||
      ::setsockopt(descriptor.get(), SOL_SOCKET, SO_SNDBUF, &send_buffer_bytes,
                   sizeof(send_buffer_bytes)) != 0) {
    FailErrno("configure Manager scene client socket buffers");
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, spelling.c_str(), spelling.size() + 1U);
  if (::connect(descriptor.get(), reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
    Require(errno == EINPROGRESS || errno == EAGAIN,
            std::string("connect to Manager command socket failed: ") +
                std::system_error(errno, std::generic_category()).what());
    pollfd connected{descriptor.get(), POLLOUT, 0};
    Require(!stop_token.stop_requested(), "Manager scene request cancelled");
    int ready = -1;
    do {
      ready = ::poll(&connected, 1, kManagerSceneDeadlineMilliseconds);
    } while (ready < 0 && errno == EINTR);
    Require(ready > 0 && (connected.revents & POLLOUT) != 0,
            "Manager command socket connection timed out");
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    Require(::getsockopt(descriptor.get(), SOL_SOCKET, SO_ERROR, &socket_error,
                         &error_size) == 0 &&
                error_size == sizeof(socket_error) && socket_error == 0,
            "Manager command socket connection was rejected");
  }

  pollfd outbound{descriptor.get(), POLLOUT, 0};
  Require(!stop_token.stop_requested(), "Manager scene request cancelled");
  int ready = -1;
  do {
    ready = ::poll(&outbound, 1, kManagerSceneDeadlineMilliseconds);
  } while (ready < 0 && errno == EINTR);
  Require(ready > 0 && (outbound.revents & POLLOUT) != 0,
          "Manager did not accept a scene request in time");
  ssize_t sent = -1;
  do {
    sent = ::send(descriptor.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  Require(sent == static_cast<ssize_t>(bytes.size()),
          "Manager scene request was not sent atomically");

  pollfd inbound{descriptor.get(), POLLIN, 0};
  Require(!stop_token.stop_requested(), "Manager scene request cancelled");
  do {
    ready = ::poll(&inbound, 1, kManagerSceneDeadlineMilliseconds);
  } while (ready < 0 && errno == EINTR);
  Require(ready > 0 && (inbound.revents & POLLIN) != 0,
          "Manager scene response timed out");
  std::vector<char> buffer(kMaximumManagerResponseBytes);
  iovec vector{buffer.data(), buffer.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  ssize_t received = -1;
  do {
    received = ::recvmsg(descriptor.get(), &message, 0);
  } while (received < 0 && errno == EINTR);
  Require(received > 0 && (message.msg_flags & MSG_TRUNC) == 0,
          "Manager scene response is empty or oversized");
  const std::string_view response_bytes(buffer.data(),
                                        static_cast<std::size_t>(received));
  const auto parsed = deployment::parse_strict_json_model_yaml(response_bytes);
  Require(content_identity::canonical_json(parsed.root()) == response_bytes &&
              parsed.root().is_object() &&
              Text(parsed.root(), "api_version") == kManagerResponseApiVersion,
          "Manager scene response is not the canonical supported contract");
  static_cast<void>(Boolean(parsed.root(), "ok"));
  return parsed.root();
}

[[nodiscard]] HostConfig ParseArguments(int argc, char **argv) {
  HostConfig config;
  std::set<std::string> seen;
  for (int index = 1; index < argc; index += 2) {
    Require(index + 1 < argc, "every option requires one value");
    const std::string option(argv[index]);
    const std::string value(argv[index + 1]);
    Require(seen.insert(option).second, "duplicate option " + option);
    if (option == "--package-root") {
      config.package_root = value;
    } else if (option == "--manager-state-root") {
      config.manager_state_root = value;
    } else if (option == "--execution-state-root") {
      config.execution_state_root = value;
    } else if (option == "--execution-run-root") {
      config.execution_run_root = value;
    } else if (option == "--program-binding-id") {
      config.program_binding_id = value;
    } else if (option == "--motion-target-role-id") {
      config.motion_target_role_id = value;
    } else if (option == "--gripper-target-role-id") {
      config.gripper_target_role_id = value;
    } else if (option == "--candidate-worker") {
      config.candidate_worker = value;
    } else if (option == "--cell-id") {
      config.cell_id = value;
    } else {
      Fail("unknown option " + option);
    }
  }
  const auto has = [&](const std::string_view option) {
    return seen.contains(std::string(option));
  };
  Require(has("--package-root") && has("--manager-state-root") &&
              has("--execution-state-root") &&
              has("--execution-run-root") && has("--program-binding-id") &&
              has("--candidate-worker") && has("--cell-id") &&
              (has("--motion-target-role-id") ||
               has("--gripper-target-role-id")) &&
              seen.size() <= 9U,
          "package, Manager state, execution state/run, Program Binding, "
          "candidate worker, cell, and at least one supported target role "
          "are required");
  config.package_root =
      ExistingDirectory(std::move(config.package_root), "package root");
  config.manager_state_root = ExistingPrivateDirectory(
      std::move(config.manager_state_root), "Manager state root");
  config.execution_state_root = ExistingPrivateDirectory(
      std::move(config.execution_state_root), "execution state root");
  config.execution_run_root = ExistingPrivateDirectory(
      std::move(config.execution_run_root), "execution run root");
  RequireDisjointDirectories(config.manager_state_root,
                             config.execution_state_root,
                             "standalone durable state capabilities");
  RequireDisjointDirectories(
      config.manager_state_root, config.execution_run_root,
      "standalone Manager-state/execution-run capabilities");
  RequireDisjointDirectories(config.execution_state_root,
                             config.execution_run_root,
                             "standalone execution state/run capabilities");
  RequireDisjointDirectories(config.package_root, config.manager_state_root,
                             "standalone install/Manager-state capabilities");
  RequireDisjointDirectories(config.package_root, config.execution_state_root,
                             "standalone install/execution-state capabilities");
  RequireDisjointDirectories(config.package_root, config.execution_run_root,
                             "standalone install/execution-run capabilities");
  config.candidate_worker = ExistingRegularFile(
      std::move(config.candidate_worker), "candidate worker", true);
  Require(!config.cell_id.empty() &&
              config.cell_id.size() <= kMaximumIdentityBytes &&
              !config.program_binding_id.empty() &&
              config.program_binding_id.size() <= kMaximumIdentityBytes &&
              (config.motion_target_role_id.empty() ||
               config.motion_target_role_id.size() <=
                   kMaximumIdentityBytes) &&
              (config.gripper_target_role_id.empty() ||
               config.gripper_target_role_id.size() <=
                   kMaximumIdentityBytes),
          "host identities must be bounded and non-empty");
  return config;
}

void InstallSignalHandlers() {
  if (g_stop_signal_wake_fd.load() < 0) {
    const int wake = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake < 0) {
      FailErrno("create execution-host stop-signal wake");
    }
    g_stop_signal_wake_fd.store(wake);
  }
  struct sigaction action {};
  action.sa_handler = HandleStopSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  if (::sigaction(SIGINT, &action, nullptr) != 0 ||
      ::sigaction(SIGTERM, &action, nullptr) != 0) {
    FailErrno("install execution-host signal handlers");
  }
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (::sigaction(SIGPIPE, &ignore, nullptr) != 0) {
    FailErrno("ignore execution-host SIGPIPE");
  }
}
} // namespace manyforge::roboplan_backend::host_detail
