#include "manyforge_headless/source_snapshot.hpp"

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;

class FileDescriptor final {
public:
  explicit FileDescriptor(const int value = -1) noexcept : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }

  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        static_cast<void>(::close(value_));
      }
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return value_; }

  void Close(const std::string_view context) {
    if (value_ < 0) {
      return;
    }
    const int value = std::exchange(value_, -1);
    if (::close(value) != 0) {
      throw SourceSnapshotIoError(
          std::string(context) + ": " +
          std::system_error(errno, std::generic_category()).what());
    }
  }

private:
  int value_;
};

[[noreturn]] void ThrowIo(const std::string_view context) {
  throw SourceSnapshotIoError(
      std::string(context) + ": " +
      std::system_error(errno, std::generic_category()).what());
}

void ValidateOwnerToken(const std::string_view value,
                        const std::string_view label) {
  if (value.empty() || value.size() > kMaximumSourceOwnerIdentifierBytes ||
      std::any_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x21U || byte > 0x7EU;
      })) {
    throw std::invalid_argument(std::string(label) +
                                " must be bounded printable ASCII");
  }
}

void ValidateFileName(const std::string_view value) {
  if (value.empty() || value == "." || value == ".." ||
      value.size() > kMaximumSourceOwnerIdentifierBytes ||
      value.find('/') != std::string_view::npos ||
      value.find('\0') != std::string_view::npos) {
    throw std::invalid_argument(
        "source snapshot file name must be one bounded path component");
  }
}

CanonicalValue Number(const std::uint64_t value, const std::string_view label) {
  if (value == 0U || value > kMaximumSafeInteger) {
    throw std::invalid_argument(std::string(label) +
                                " must be a nonzero safe integer");
  }
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue Number(const std::int64_t value, const std::string_view label) {
  if (value < 0) {
    throw std::invalid_argument(std::string(label) +
                                " must be a nonnegative int64");
  }
  return content_identity::nonnegative_int64_value(value);
}

CanonicalValue SemanticValue(const std::string &source_owner_kind,
                             const std::string &source_owner_id,
                             const std::string &source_owner_epoch,
                             const CanonicalValue &payload) {
  return CanonicalValue(CanonicalValue::Object{
      {"payload", payload},
      {"source_owner_epoch", CanonicalValue(source_owner_epoch)},
      {"source_owner_id", CanonicalValue(source_owner_id)},
      {"source_owner_kind", CanonicalValue(source_owner_kind)},
  });
}

std::string SemanticDigest(const std::string &source_owner_kind,
                           const std::string &source_owner_id,
                           const std::string &source_owner_epoch,
                           const CanonicalValue &payload) {
  return content_identity::framed_sha256(
      kSourceSnapshotSemanticIdentityDomain, kSourceSnapshotApiVersion,
      SemanticValue(source_owner_kind, source_owner_id, source_owner_epoch,
                    payload));
}

CanonicalValue RecordValueWithoutDigest(const SourceSnapshot &snapshot) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kSourceSnapshotApiVersion))},
      {"observed_at_ns", Number(snapshot.observed_at_ns, "observed_at_ns")},
      {"payload", snapshot.payload},
      {"semantic_content_digest",
       CanonicalValue(snapshot.semantic_content_digest)},
      {"source_generation",
       Number(snapshot.source_generation, "source_generation")},
      {"source_owner_epoch", CanonicalValue(snapshot.source_owner_epoch)},
      {"source_owner_id", CanonicalValue(snapshot.source_owner_id)},
      {"source_owner_kind", CanonicalValue(snapshot.source_owner_kind)},
  });
}

std::string RecordDigest(const SourceSnapshot &snapshot) {
  return content_identity::framed_sha256(kSourceSnapshotRecordIdentityDomain,
                                         kSourceSnapshotApiVersion,
                                         RecordValueWithoutDigest(snapshot));
}

CanonicalValue CompleteRecordValue(const SourceSnapshot &snapshot) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kSourceSnapshotApiVersion))},
      {"observed_at_ns", Number(snapshot.observed_at_ns, "observed_at_ns")},
      {"payload", snapshot.payload},
      {"record_digest", CanonicalValue(snapshot.record_digest)},
      {"semantic_content_digest",
       CanonicalValue(snapshot.semantic_content_digest)},
      {"source_generation",
       Number(snapshot.source_generation, "source_generation")},
      {"source_owner_epoch", CanonicalValue(snapshot.source_owner_epoch)},
      {"source_owner_id", CanonicalValue(snapshot.source_owner_id)},
      {"source_owner_kind", CanonicalValue(snapshot.source_owner_kind)},
  });
}

const CanonicalValue &RequiredMember(const CanonicalValue &root,
                                     const std::string_view name) {
  const auto *member = root.find_object_member(name);
  if (member == nullptr) {
    throw SourceSnapshotIntegrityError("source snapshot is missing field " +
                                       std::string(name));
  }
  return *member;
}

std::string RequiredString(const CanonicalValue &root,
                           const std::string_view name) {
  const auto &storage = RequiredMember(root, name).storage();
  const auto *value = std::get_if<std::string>(&storage);
  if (value == nullptr) {
    throw SourceSnapshotIntegrityError("source snapshot field " +
                                       std::string(name) + " must be a string");
  }
  return *value;
}

std::uint64_t RequiredUnsigned(const CanonicalValue &root,
                               const std::string_view name,
                               const bool nonzero) {
  const auto &storage = RequiredMember(root, name).storage();
  const auto *value = std::get_if<double>(&storage);
  if (value == nullptr || !std::isfinite(*value) || *value < 0.0 ||
      std::floor(*value) != *value || *value > kMaximumSafeInteger ||
      (nonzero && *value == 0.0)) {
    throw SourceSnapshotIntegrityError("source snapshot field " +
                                       std::string(name) +
                                       " must be a safe integer");
  }
  return static_cast<std::uint64_t>(*value);
}

FileDescriptor OpenDirectory(const std::filesystem::path &directory) {
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY |
                                                       O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    ThrowIo("open source snapshot directory");
  }
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    const int error = errno;
    static_cast<void>(::close(descriptor));
    errno = error;
    ThrowIo("inspect source snapshot directory");
  }
  if (!S_ISDIR(status.st_mode)) {
    static_cast<void>(::close(descriptor));
    throw SourceSnapshotIoError(
        "source snapshot publication root is not a directory");
  }
  return FileDescriptor(descriptor);
}

std::optional<SourceSnapshotFileObservation>
ReadObservedAt(const int directory_fd, const std::string &file_name) {
  const int descriptor = ::openat(directory_fd, file_name.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return std::nullopt;
    }
    ThrowIo("open source snapshot publication");
  }
  FileDescriptor file(descriptor);
  struct stat status {};
  if (::fstat(file.get(), &status) != 0) {
    ThrowIo("inspect source snapshot publication");
  }
  if (!S_ISREG(status.st_mode) || status.st_size <= 0 ||
      static_cast<std::uint64_t>(status.st_size) >
          kMaximumSourceSnapshotBytes) {
    throw SourceSnapshotIntegrityError(
        "source snapshot publication is not one bounded regular file");
  }

  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(status.st_size));
  char buffer[8192];
  for (;;) {
    const auto count = ::read(file.get(), buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      ThrowIo("read source snapshot publication");
    }
    if (count == 0) {
      break;
    }
    bytes.append(buffer, static_cast<std::size_t>(count));
    if (bytes.size() > kMaximumSourceSnapshotBytes) {
      throw SourceSnapshotIntegrityError(
          "source snapshot publication exceeds its byte bound");
    }
  }
  if (bytes.size() != static_cast<std::size_t>(status.st_size)) {
    throw SourceSnapshotIntegrityError(
        "source snapshot publication size changed while reading");
  }
  constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000LL;
  if (status.st_mtim.tv_sec < 0 || status.st_mtim.tv_nsec < 0 ||
      status.st_mtim.tv_nsec >= kNanosecondsPerSecond ||
      status.st_mtim.tv_sec >
          (std::numeric_limits<std::int64_t>::max() - status.st_mtim.tv_nsec) /
              kNanosecondsPerSecond) {
    throw SourceSnapshotIntegrityError(
        "source snapshot publication mtime is invalid");
  }
  return SourceSnapshotFileObservation{
      SourceSnapshotCodec::Decode(bytes),
      status.st_mtim.tv_sec * kNanosecondsPerSecond + status.st_mtim.tv_nsec,
  };
}

void WriteAll(const int descriptor, const std::string_view bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      ThrowIo("write source snapshot temporary publication");
    }
    offset += static_cast<std::size_t>(count);
  }
}

void PublishAt(const int directory_fd, const std::string &file_name,
               const std::string_view bytes) {
  static std::atomic<std::uint64_t> temporary_sequence{0U};
  constexpr std::size_t kMaximumTemporaryNameAttempts = 64U;
  std::string temporary;
  int descriptor = -1;
  for (std::size_t attempt = 0U; attempt < kMaximumTemporaryNameAttempts;
       ++attempt) {
    temporary = "." + file_name + ".tmp." + std::to_string(::getpid()) + "." +
                std::to_string(temporary_sequence.fetch_add(1U));
    descriptor =
        ::openat(directory_fd, temporary.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0640);
    if (descriptor >= 0 || errno != EEXIST) {
      break;
    }
  }
  if (descriptor < 0) {
    ThrowIo("create source snapshot temporary publication");
  }
  FileDescriptor file(descriptor);
  bool renamed = false;
  try {
    WriteAll(file.get(), bytes);
    if (::fsync(file.get()) != 0) {
      ThrowIo("fsync source snapshot temporary publication");
    }
    file.Close("close source snapshot temporary publication");
    if (::renameat(directory_fd, temporary.c_str(), directory_fd,
                   file_name.c_str()) != 0) {
      ThrowIo("atomically select source snapshot publication");
    }
    renamed = true;
    if (::fsync(directory_fd) != 0) {
      ThrowIo("fsync source snapshot publication directory");
    }
  } catch (...) {
    if (!renamed) {
      static_cast<void>(::unlinkat(directory_fd, temporary.c_str(), 0));
    }
    throw;
  }
}

} // namespace

bool SourceSnapshot::valid() const noexcept {
  const auto bounded_printable = [](const std::string &value) {
    return !value.empty() &&
           value.size() <= kMaximumSourceOwnerIdentifierBytes &&
           std::all_of(value.begin(), value.end(), [](const char character) {
             const auto byte = static_cast<unsigned char>(character);
             return byte >= 0x21U && byte <= 0x7EU;
           });
  };
  return bounded_printable(source_owner_kind) &&
         bounded_printable(source_owner_id) &&
         bounded_printable(source_owner_epoch) && source_generation != 0U &&
         source_generation <= kMaximumSafeInteger && observed_at_ns >= 0 &&
         content_identity::is_sha256_digest(semantic_content_digest) &&
         content_identity::is_sha256_digest(record_digest);
}

SourceSnapshot SourceSnapshotCodec::Create(
    std::string source_owner_kind, std::string source_owner_id,
    std::string source_owner_epoch, const std::uint64_t source_generation,
    const std::int64_t observed_at_ns, CanonicalValue payload) {
  ValidateOwnerToken(source_owner_kind, "source_owner_kind");
  ValidateOwnerToken(source_owner_id, "source_owner_id");
  ValidateOwnerToken(source_owner_epoch, "source_owner_epoch");
  static_cast<void>(Number(source_generation, "source_generation"));
  static_cast<void>(Number(observed_at_ns, "observed_at_ns"));

  SourceSnapshot result{
      std::move(source_owner_kind),
      std::move(source_owner_id),
      std::move(source_owner_epoch),
      source_generation,
      observed_at_ns,
      std::move(payload),
      {},
      {},
  };
  result.semantic_content_digest =
      SemanticDigest(result.source_owner_kind, result.source_owner_id,
                     result.source_owner_epoch, result.payload);
  result.record_digest = RecordDigest(result);
  if (!result.valid()) {
    throw std::invalid_argument("constructed source snapshot is invalid");
  }
  return result;
}

std::string SourceSnapshotCodec::Encode(const SourceSnapshot &snapshot) {
  if (!snapshot.valid() ||
      snapshot.semantic_content_digest !=
          SemanticDigest(snapshot.source_owner_kind, snapshot.source_owner_id,
                         snapshot.source_owner_epoch, snapshot.payload) ||
      snapshot.record_digest != RecordDigest(snapshot)) {
    throw SourceSnapshotIntegrityError(
        "source snapshot does not match its semantic and record digests");
  }
  const auto bytes =
      content_identity::canonical_json(CompleteRecordValue(snapshot));
  if (bytes.empty() || bytes.size() > kMaximumSourceSnapshotBytes) {
    throw SourceSnapshotIntegrityError(
        "encoded source snapshot exceeds its byte bound");
  }
  return bytes;
}

SourceSnapshot
SourceSnapshotCodec::Decode(const std::string_view canonical_bytes) {
  if (canonical_bytes.empty() ||
      canonical_bytes.size() > kMaximumSourceSnapshotBytes) {
    throw SourceSnapshotIntegrityError(
        "source snapshot bytes violate the fixed bound");
  }

  CanonicalValue root;
  try {
    root = deployment::parse_strict_json_model_yaml(canonical_bytes).root();
  } catch (const std::exception &error) {
    throw SourceSnapshotIntegrityError("source snapshot is not strict JSON: " +
                                       std::string(error.what()));
  }
  if (!root.is_object() || root.as_object().size() != 9U ||
      content_identity::canonical_json(root) != canonical_bytes) {
    throw SourceSnapshotIntegrityError(
        "source snapshot must be one exact canonical closed v1 object");
  }
  if (RequiredString(root, "api_version") != kSourceSnapshotApiVersion) {
    throw SourceSnapshotIntegrityError(
        "source snapshot api_version is unsupported");
  }

  const auto generation = RequiredUnsigned(root, "source_generation", true);
  std::int64_t observed{};
  try {
    observed = content_identity::parse_nonnegative_int64(RequiredMember(root, "observed_at_ns"));
  } catch (const content_identity::CanonicalValueError&) {
    throw SourceSnapshotIntegrityError(
        "source snapshot observed_at_ns is not exact nonnegative int64");
  }
  auto result = Create(RequiredString(root, "source_owner_kind"),
                       RequiredString(root, "source_owner_id"),
                       RequiredString(root, "source_owner_epoch"), generation,
                       observed,
                       RequiredMember(root, "payload"));
  if (result.semantic_content_digest !=
          RequiredString(root, "semantic_content_digest") ||
      result.record_digest != RequiredString(root, "record_digest")) {
    throw SourceSnapshotIntegrityError(
        "source snapshot digest verification failed");
  }
  return result;
}

AtomicSourceSnapshotChannel::AtomicSourceSnapshotChannel(
    std::filesystem::path directory, std::string file_name)
    : directory_(std::move(directory)), file_name_(std::move(file_name)) {
  if (directory_.empty()) {
    throw std::invalid_argument(
        "source snapshot publication directory must be non-empty");
  }
  ValidateFileName(file_name_);
}

const std::filesystem::path &
AtomicSourceSnapshotChannel::directory() const noexcept {
  return directory_;
}

const std::string &AtomicSourceSnapshotChannel::file_name() const noexcept {
  return file_name_;
}

std::optional<SourceSnapshot> AtomicSourceSnapshotChannel::Read() const {
  const auto observed = ReadObserved();
  if (!observed.has_value()) {
    return std::nullopt;
  }
  return observed->snapshot;
}

std::optional<SourceSnapshotFileObservation>
AtomicSourceSnapshotChannel::ReadObserved() const {
  std::lock_guard lock(mutex_);
  auto directory = OpenDirectory(directory_);
  return ReadObservedAt(directory.get(), file_name_);
}

SourceSnapshot AtomicSourceSnapshotChannel::Publish(
    std::string source_owner_kind, std::string source_owner_id,
    std::string source_owner_epoch, const std::int64_t observed_at_ns,
    CanonicalValue payload) {
  ValidateOwnerToken(source_owner_kind, "source_owner_kind");
  ValidateOwnerToken(source_owner_id, "source_owner_id");
  ValidateOwnerToken(source_owner_epoch, "source_owner_epoch");
  static_cast<void>(Number(observed_at_ns, "observed_at_ns"));

  std::lock_guard lock(mutex_);
  auto directory = OpenDirectory(directory_);
  const auto current = ReadObservedAt(directory.get(), file_name_);
  const auto semantic_digest = SemanticDigest(
      source_owner_kind, source_owner_id, source_owner_epoch, payload);
  std::uint64_t generation = 1U;
  if (current.has_value()) {
    if (current->snapshot.source_owner_kind != source_owner_kind ||
        current->snapshot.source_owner_id != source_owner_id) {
      throw SourceSnapshotIntegrityError(
          "source snapshot channel is already bound to another owner");
    }
    if (current->snapshot.source_owner_epoch == source_owner_epoch) {
      generation = current->snapshot.source_generation;
      if (current->snapshot.semantic_content_digest != semantic_digest) {
        if (generation >= kMaximumSafeInteger) {
          throw SourceSnapshotIntegrityError(
              "source snapshot generation is exhausted");
        }
        ++generation;
      }
    }
  }

  auto next = SourceSnapshotCodec::Create(
      std::move(source_owner_kind), std::move(source_owner_id),
      std::move(source_owner_epoch), generation, observed_at_ns,
      std::move(payload));
  PublishAt(directory.get(), file_name_, SourceSnapshotCodec::Encode(next));
  return next;
}

} // namespace manyforge::headless
