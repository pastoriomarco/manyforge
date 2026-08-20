#include "manager_state_marker_internal.hpp"

#include "manyforge_component_manager/durable_store_set.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

namespace manyforge::component_manager {
namespace {

using content_identity::CanonicalValue;

constexpr std::string_view kMarkerApiVersion =
    "manyforge.manager_state_marker.v1";
constexpr std::string_view kManagerDaemonLockFilename =
    ".manager-daemon.lock";
constexpr std::string_view kManagerCommandSocketFilename =
    "manager-command.sock";
constexpr std::string_view kMarkerTemporaryFilename =
    ".manager-state-marker.json.tmp";

[[noreturn]] void ThrowMarkerIntegrity(std::string reason) {
  throw ManagerStateMarkerIntegrityError(core::ErrorDTO{
      "manager_state_marker_integrity_error",
      "Manager state marker is malformed or does not match the selected "
      "canonical-empty admission",
      {{"reason", std::move(reason)}, {"retryable", "false"}}});
}

[[noreturn]] void ThrowMarkerMissing() {
  throw ManagerStateMarkerMissingError(core::ErrorDTO{
      "runtime_compatibility_manifest_missing",
      "Manager state predates the manifest-native Manager runtime; a clean "
      "canonical bootstrap is required",
      {{"required_action", "clean_canonical_bootstrap"},
       {"retryable", "false"}}});
}

[[nodiscard]] std::string ReadBoundedMarker(
    const std::filesystem::path& path) {
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    ThrowMarkerIntegrity("marker cannot be opened without following a "
                         "symlink");
  }
  struct DescriptorCloser final {
    int value;
    ~DescriptorCloser() {
      if (value >= 0) {
        static_cast<void>(::close(value));
      }
    }
  } closer{descriptor};

  struct stat status {};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
    ThrowMarkerIntegrity("marker is not a regular file");
  }
  if (status.st_size < 0 ||
      static_cast<std::uintmax_t>(status.st_size) >
          kMaximumManagerStateMarkerBytes) {
    ThrowMarkerIntegrity("marker exceeds its bounded size");
  }

  std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count =
        ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowMarkerIntegrity("marker read failed");
    }
    if (count == 0) {
      ThrowMarkerIntegrity("marker changed during bounded read");
    }
    offset += static_cast<std::size_t>(count);
  }
  return bytes;
}

[[nodiscard]] const std::string& MarkerString(
    const CanonicalValue& object,
    const std::string_view name) {
  const auto* member = object.find_object_member(name);
  if (member == nullptr) {
    ThrowMarkerIntegrity("marker is missing " + std::string(name));
  }
  const auto* text = std::get_if<std::string>(&member->storage());
  if (text == nullptr || text->empty()) {
    ThrowMarkerIntegrity("marker field is not a non-empty string: " +
                         std::string(name));
  }
  return *text;
}

[[nodiscard]] std::string MarkerBytes(
    const ManagerStateMarkerIdentity& identity) {
  return content_identity::canonical_json(CanonicalValue(
      CanonicalValue::Object{
          {"admission_digest",
           CanonicalValue(identity.admission_digest)},
          {"api_version", CanonicalValue(std::string(kMarkerApiVersion))},
          {"cell_id", CanonicalValue(identity.cell_id)},
          {"graph_revision", CanonicalValue(identity.graph_revision)},
          {"retained_bundle_id",
           CanonicalValue(identity.retained_bundle_id)},
      }));
}

[[nodiscard]] bool IsRecognizedManagerEntry(
    const std::string_view filename) {
  if (filename == kManagerStateMarkerFilename ||
      filename == kManagerDaemonLockFilename ||
      filename == kManagerCommandSocketFilename) {
    return true;
  }
  const std::string names[]{
      std::string(kManagerBootstrapJournalFilename),
      std::string(kActiveBundlePublicationStoreFilename),
      std::string(kManagerApplyJournalFilename),
  };
  for (const auto& name : names) {
    if (filename == name || filename == name + "-wal" ||
        filename == name + "-shm" || filename == name + "-journal") {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool HasUnsafePreManifestEntry(
    const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::directory_iterator entries(directory, error);
  if (error) {
    ThrowMarkerIntegrity("cannot enumerate Manager state root");
  }
  for (const auto& entry : entries) {
    const auto filename = entry.path().filename().string();
    if (filename == kManagerDaemonLockFilename) {
      // ManagerDaemon owns this outer lock and may create it before this
      // store-set composition root. It is not durable Manager state.
      std::error_code status_error;
      const auto status =
          std::filesystem::symlink_status(entry.path(), status_error);
      if (status_error || std::filesystem::is_symlink(status) ||
          !std::filesystem::is_regular_file(status)) {
        return true;
      }
      continue;
    }
    return true;
  }
  return false;
}

void VerifyNoUnknownManifestNativeEntries(
    const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::directory_iterator entries(directory, error);
  if (error) {
    ThrowMarkerIntegrity("cannot enumerate Manager state root");
  }
  for (const auto& entry : entries) {
    if (!IsRecognizedManagerEntry(entry.path().filename().string())) {
      ThrowMarkerIntegrity("Manager state root contains an unrecognized "
                           "entry");
    }
  }
}

[[nodiscard]] bool MarkerExists(
    const std::filesystem::path& path,
    std::filesystem::file_status* status) {
  std::error_code error;
  *status = std::filesystem::symlink_status(path, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return false;
    }
    ThrowMarkerIntegrity("cannot inspect Manager state marker");
  }
  return status->type() != std::filesystem::file_type::not_found;
}

void WriteAll(const int descriptor, const std::string_view bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowMarkerIntegrity("cannot write Manager state marker");
    }
    if (count == 0) {
      ThrowMarkerIntegrity("Manager state marker write made no progress");
    }
    offset += static_cast<std::size_t>(count);
  }
}

void FsyncDirectory(const std::filesystem::path& directory) {
  const int descriptor =
      ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                  O_NOFOLLOW);
  if (descriptor < 0) {
    ThrowMarkerIntegrity("cannot open Manager state root for marker fsync");
  }
  if (::fsync(descriptor) != 0) {
    const auto detail = std::string(std::strerror(errno));
    static_cast<void>(::close(descriptor));
    ThrowMarkerIntegrity("cannot fsync Manager state marker directory: " +
                         detail);
  }
  if (::close(descriptor) != 0) {
    ThrowMarkerIntegrity("cannot close Manager state marker directory");
  }
}

}  // namespace

ManagerStateMarkerPreflightResult PreflightManagerStateMarker(
    const std::filesystem::path& stable_state_directory,
    const ManagerStateMarkerIdentity& expected) {
  const auto marker_path =
      stable_state_directory / std::string(kManagerStateMarkerFilename);
  std::filesystem::file_status marker_status;
  if (!MarkerExists(marker_path, &marker_status)) {
    if (HasUnsafePreManifestEntry(stable_state_directory)) {
      ThrowMarkerMissing();
    }
    return ManagerStateMarkerPreflightResult::kClean;
  }
  if (std::filesystem::is_symlink(marker_status) ||
      !std::filesystem::is_regular_file(marker_status)) {
    ThrowMarkerIntegrity("Manager state marker is not a regular non-symlink "
                         "file");
  }

  const auto bytes = ReadBoundedMarker(marker_path);
  CanonicalValue root;
  try {
    root = deployment::parse_strict_json_model_yaml(bytes).root();
  } catch (const std::exception&) {
    ThrowMarkerIntegrity("Manager state marker is not strict canonical JSON");
  }
  if (!root.is_object() || root.as_object().size() != 5U ||
      content_identity::canonical_json(root) != bytes) {
    ThrowMarkerIntegrity("Manager state marker has an unknown field or "
                         "non-canonical encoding");
  }
  if (MarkerString(root, "api_version") != kMarkerApiVersion ||
      MarkerString(root, "admission_digest") != expected.admission_digest ||
      MarkerString(root, "cell_id") != expected.cell_id ||
      MarkerString(root, "graph_revision") != expected.graph_revision ||
      MarkerString(root, "retained_bundle_id") !=
          expected.retained_bundle_id) {
    ThrowMarkerIntegrity("Manager state marker identity does not match the "
                         "selected canonical-empty admission");
  }
  VerifyNoUnknownManifestNativeEntries(stable_state_directory);
  return ManagerStateMarkerPreflightResult::kValid;
}

void WriteManagerStateMarker(
    const std::filesystem::path& stable_state_directory,
    const ManagerStateMarkerIdentity& identity) {
  const auto bytes = MarkerBytes(identity);
  if (bytes.empty() || bytes.size() > kMaximumManagerStateMarkerBytes) {
    ThrowMarkerIntegrity("generated Manager state marker exceeds its bound");
  }
  const auto temporary_path =
      stable_state_directory / std::string(kMarkerTemporaryFilename);
  const auto marker_path =
      stable_state_directory / std::string(kManagerStateMarkerFilename);
  const int descriptor =
      ::open(temporary_path.c_str(),
             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
             S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    ThrowMarkerIntegrity("cannot create Manager state marker temporary file");
  }
  bool closed = false;
  try {
    WriteAll(descriptor, bytes);
    if (::fsync(descriptor) != 0) {
      ThrowMarkerIntegrity("cannot fsync Manager state marker");
    }
    if (::close(descriptor) != 0) {
      ThrowMarkerIntegrity("cannot close Manager state marker");
    }
    closed = true;
    if (::rename(temporary_path.c_str(), marker_path.c_str()) != 0) {
      ThrowMarkerIntegrity("cannot publish Manager state marker");
    }
    FsyncDirectory(stable_state_directory);
  } catch (...) {
    if (!closed) {
      static_cast<void>(::close(descriptor));
    }
    throw;
  }
}

}  // namespace manyforge::component_manager
