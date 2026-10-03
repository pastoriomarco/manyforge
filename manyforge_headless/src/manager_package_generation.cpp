#include "manyforge_headless/manager_package_generation.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <map>
#include <system_error>
#include <utility>

#include <manyforge_core/runtime_contract_manifest_codec.hpp>

namespace manyforge::headless {
namespace {

using manyforge::core::ErrorDTO;
using manyforge::core::RuntimeContractManifestSetDTO;

[[nodiscard]] std::filesystem::path
CanonicalPlainDirectory(std::filesystem::path path) {
  std::error_code error;
  const auto direct = std::filesystem::symlink_status(path, error);
  if (error || std::filesystem::is_symlink(direct) ||
      !std::filesystem::is_directory(direct)) {
    throw std::invalid_argument(
        "Manager package root must be an existing non-symlink directory");
  }
  auto canonical = std::filesystem::canonical(path, error);
  if (error || !canonical.is_absolute()) {
    throw std::invalid_argument("Manager package root is not canonical");
  }
  const auto canonical_status =
      std::filesystem::symlink_status(canonical, error);
  if (error || std::filesystem::is_symlink(canonical_status) ||
      !std::filesystem::is_directory(canonical_status)) {
    throw std::invalid_argument("canonical Manager package root is invalid");
  }
  return canonical;
}

[[nodiscard]] int OpenRoot(const std::filesystem::path &root) {
  return ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

[[nodiscard]] struct stat Inspect(const int descriptor,
                                  const char *operation) {
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    throw std::system_error(errno, std::generic_category(), operation);
  }
  if (!S_ISDIR(status.st_mode)) {
    throw std::invalid_argument("Manager package root is not a directory");
  }
  return status;
}

} // namespace

ManagerPackageGenerationRestartRequired::
    ManagerPackageGenerationRestartRequired(ErrorDTO error)
    : std::runtime_error(error.message), error_(std::move(error)) {}

const ErrorDTO &
ManagerPackageGenerationRestartRequired::error() const noexcept {
  return error_;
}

ManagerPackageGeneration::ManagerPackageGeneration(
    std::filesystem::path package_root,
    RuntimeContractManifestSetDTO manifest_set)
    : package_root_(CanonicalPlainDirectory(std::move(package_root))),
      manifest_set_(std::move(manifest_set)) {
  const auto verified =
      manyforge::core::VerifyRuntimeContractManifestSet(manifest_set_);
  if (!verified.ok()) {
    throw std::invalid_argument(
        "Manager package manifest-set snapshot is invalid: " +
        verified.error().message);
  }
  root_descriptor_ = OpenRoot(package_root_);
  if (root_descriptor_ < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "open Manager package root");
  }
  struct stat status {};
  try {
    status = Inspect(root_descriptor_, "inspect Manager package root");
  } catch (...) {
    static_cast<void>(::close(root_descriptor_));
    root_descriptor_ = -1;
    throw;
  }
  root_device_ = static_cast<std::uint64_t>(status.st_dev);
  root_inode_ = static_cast<std::uint64_t>(status.st_ino);
}

ManagerPackageGeneration::~ManagerPackageGeneration() {
  if (root_descriptor_ >= 0) {
    static_cast<void>(::close(root_descriptor_));
  }
}

const std::filesystem::path &
ManagerPackageGeneration::package_root() const noexcept {
  return package_root_;
}

std::filesystem::path ManagerPackageGeneration::descriptor_bound_root() const {
  if (root_descriptor_ < 0) {
    throw std::logic_error("Manager package root descriptor is closed");
  }
  struct stat status {};
  if (::fstat(root_descriptor_, &status) != 0 || !S_ISDIR(status.st_mode)) {
    throw std::logic_error("Manager package root descriptor is not a directory");
  }
  return std::filesystem::path("/proc/self/fd") /
         std::to_string(root_descriptor_);
}

const RuntimeContractManifestSetDTO &
ManagerPackageGeneration::manifest_set() const noexcept {
  return manifest_set_;
}

void ManagerPackageGeneration::RequireCurrent(
    const RuntimeContractManifestSetDTO &observed) {
  if (latched_error_.has_value()) {
    throw ManagerPackageGenerationRestartRequired(*latched_error_);
  }
  // The pinned set was digest-verified at construction and verification is a
  // pure function of the value, so an exactly equal observation is already
  // verified. Only a differing set is re-verified, to decide whether its
  // digest may be reported.
  if (observed != manifest_set_) {
    const auto verified =
        manyforge::core::VerifyRuntimeContractManifestSet(observed);
    LatchAndThrow("manifest_set_mismatch",
                  verified.ok()
                      ? std::optional{observed.manifest_set_digest}
                      : std::nullopt);
  }

  try {
    const auto retained =
        Inspect(root_descriptor_, "inspect pinned Manager package root");
    const int current_descriptor = OpenRoot(package_root_);
    if (current_descriptor < 0) {
      LatchAndThrow("snapshot_integrity_lost");
    }
    struct DescriptorCloser final {
      int value;
      ~DescriptorCloser() { static_cast<void>(::close(value)); }
    } closer{current_descriptor};
    const auto current =
        Inspect(current_descriptor, "inspect current Manager package root");
    if (static_cast<std::uint64_t>(retained.st_dev) != root_device_ ||
        static_cast<std::uint64_t>(retained.st_ino) != root_inode_ ||
        static_cast<std::uint64_t>(current.st_dev) != root_device_ ||
        static_cast<std::uint64_t>(current.st_ino) != root_inode_) {
      LatchAndThrow("snapshot_integrity_lost");
    }
  } catch (const ManagerPackageGenerationRestartRequired &) {
    throw;
  } catch (...) {
    LatchAndThrow("snapshot_integrity_lost");
  }
}

[[noreturn]] void ManagerPackageGeneration::LatchAndThrow(
    std::string reason,
    std::optional<manyforge::core::RuntimeContractManifestSetDigest>
        observed_digest) {
  std::map<std::string, std::string> details{
      {"expected_manifest_set_digest",
       manifest_set_.manifest_set_digest.value()},
      {"reason", std::move(reason)},
      {"required_action", "complete_stack_stop_start"},
      {"retryable", "false"},
  };
  if (observed_digest.has_value()) {
    details.emplace("observed_manifest_set_digest",
                    observed_digest->value());
  }
  latched_error_ = ErrorDTO{
      "full_stack_restart_required",
      "Manager package generation changed; complete stack restart required",
      std::move(details)};
  throw ManagerPackageGenerationRestartRequired(*latched_error_);
}

} // namespace manyforge::headless
