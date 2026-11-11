#include "manyforge_headless/source_snapshot.hpp"

#include <manyforge_content_identity/content_identity.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <limits>
#include <optional>
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
ReadObservedAt(const int directory_fd, const std::string &file_name,
               std::optional<AtomicSourceSnapshotChannel::VerifiedRecord>
                   &last_verified,
               std::uint64_t &decode_count) {
  const int descriptor =
      ::openat(directory_fd, file_name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
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
  const auto modified_at_ns =
      status.st_mtim.tv_sec * kNanosecondsPerSecond + status.st_mtim.tv_nsec;
  // Byte-identical to the last completely verified record: its decoded value
  // and both digests are a pure function of these exact bytes.
  if (last_verified.has_value() && last_verified->bytes == bytes) {
    return SourceSnapshotFileObservation{last_verified->snapshot,
                                         modified_at_ns};
  }
  last_verified.reset();
  auto snapshot = SourceSnapshotCodec::Decode(bytes);
  ++decode_count;
  last_verified.emplace(
      AtomicSourceSnapshotChannel::VerifiedRecord{std::move(bytes), snapshot});
  return SourceSnapshotFileObservation{std::move(snapshot), modified_at_ns};
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
  try {
    WriteAll(file.get(), bytes);
    file.Close("close source snapshot temporary publication");
    // No fsync: the rename alone makes every reader see the old or the new
    // complete inode. These are status files rebuilt at every start; a torn
    // file left by a power loss is replaced then (manager_daemon.cpp).
    if (::renameat(directory_fd, temporary.c_str(), directory_fd,
                   file_name.c_str()) != 0) {
      ThrowIo("atomically select source snapshot publication");
    }
  } catch (...) {
    static_cast<void>(::unlinkat(directory_fd, temporary.c_str(), 0));
    throw;
  }
}

SourceSnapshot CreateRecord(std::string source_owner_kind,
                            std::string source_owner_id,
                            std::string source_owner_epoch,
                            const std::uint64_t source_generation,
                            const std::int64_t observed_at_ns,
                            CanonicalValue payload,
                            std::optional<std::string> semantic_digest) {
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
  // A caller-supplied semantic digest was derived by the caller from these
  // exact owner identities and payload; it is not derived a second time.
  result.semantic_content_digest =
      semantic_digest.has_value()
          ? std::move(*semantic_digest)
          : SemanticDigest(result.source_owner_kind, result.source_owner_id,
                           result.source_owner_epoch, result.payload);
  result.record_digest = RecordDigest(result);
  if (!result.valid()) {
    throw std::invalid_argument("constructed source snapshot is invalid");
  }
  return result;
}

/// Encodes a record whose digests were derived by CreateRecord in this call
/// chain. Public Encode re-derives both digests for caller-supplied values.
std::string EncodeCreated(const SourceSnapshot &snapshot) {
  auto bytes = content_identity::canonical_json(CompleteRecordValue(snapshot));
  if (bytes.empty() || bytes.size() > kMaximumSourceSnapshotBytes) {
    throw SourceSnapshotIntegrityError(
        "encoded source snapshot exceeds its byte bound");
  }
  return bytes;
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
  return CreateRecord(std::move(source_owner_kind), std::move(source_owner_id),
                      std::move(source_owner_epoch), source_generation,
                      observed_at_ns, std::move(payload), std::nullopt);
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

  // A record is exactly its canonical bytes, so the canonical parser (which
  // rejects every spelling that does not reserialize byte for byte) is the
  // whole syntax check. Readers decode every changed liveness pulse; the
  // general YAML/JSON artifact decoder cost about twice as much here.
  CanonicalValue root;
  try {
    root = content_identity::parse_canonical_json(
        canonical_bytes, content_identity::CanonicalJsonParseLimits{
                             kMaximumSourceSnapshotBytes,
                             content_identity::kMaximumCanonicalJsonDepth,
                             content_identity::kMaximumCanonicalJsonNodes});
  } catch (const std::exception &error) {
    throw SourceSnapshotIntegrityError(
        "source snapshot is not canonical JSON: " + std::string(error.what()));
  }
  if (!root.is_object() || root.as_object().size() != 9U) {
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
  if (RequiredMember(root, "observed_at_ns") !=
      Number(observed, "observed_at_ns")) {
    throw SourceSnapshotIntegrityError(
        "source snapshot observed_at_ns is not its producer spelling");
  }
  SourceSnapshot result{
      RequiredString(root, "source_owner_kind"),
      RequiredString(root, "source_owner_id"),
      RequiredString(root, "source_owner_epoch"),
      generation,
      observed,
      RequiredMember(root, "payload"),
      RequiredString(root, "semantic_content_digest"),
      RequiredString(root, "record_digest"),
  };
  try {
    ValidateOwnerToken(result.source_owner_kind, "source_owner_kind");
    ValidateOwnerToken(result.source_owner_id, "source_owner_id");
    ValidateOwnerToken(result.source_owner_epoch, "source_owner_epoch");
  } catch (const std::invalid_argument &error) {
    throw SourceSnapshotIntegrityError(error.what());
  }
  if (!result.valid()) {
    throw SourceSnapshotIntegrityError("source snapshot fields are invalid");
  }

  // The bytes are exactly canonical_json(root), and a canonical object is its
  // sorted members' serializations joined by commas. Both digest preimages
  // are therefore spans of these verified bytes plus re-serialized scalars;
  // the payload is hashed as received instead of being copied and
  // re-serialized twice per read.
  const auto member = [&root](const std::string_view name) {
    return content_identity::canonical_json(CanonicalValue(std::string(name))) +
           ":" + content_identity::canonical_json(RequiredMember(root, name));
  };
  // Appending (not `"literal" + std::string`) also avoids GCC 12's false
  // -Wrestrict positive in Release builds.
  const auto join = [](std::initializer_list<std::string_view> parts) {
    std::string joined;
    for (const auto part : parts) {
      joined.append(part);
    }
    return joined;
  };
  const auto prefix = join({"{", member("api_version"), ",",
                            member("observed_at_ns"), ",\"payload\":"});
  const auto owners =
      join({",", member("source_owner_epoch"), ",", member("source_owner_id"),
            ",", member("source_owner_kind"), "}"});
  const auto after_record_digest =
      join({",", member("semantic_content_digest"), ",",
            member("source_generation"), owners});
  const auto suffix = join({",", member("record_digest"), after_record_digest});
  if (canonical_bytes.size() <= prefix.size() + suffix.size() ||
      !canonical_bytes.starts_with(prefix) ||
      !canonical_bytes.ends_with(suffix)) {
    throw SourceSnapshotIntegrityError(
        "source snapshot members are not in canonical record order");
  }
  const auto payload_bytes = canonical_bytes.substr(
      prefix.size(), canonical_bytes.size() - prefix.size() - suffix.size());
  std::string semantic_preimage;
  semantic_preimage.reserve(payload_bytes.size() + owners.size() + 16U);
  semantic_preimage.append("{\"payload\":").append(payload_bytes).append(owners);
  std::string record_preimage;
  record_preimage.reserve(prefix.size() + payload_bytes.size() +
                          after_record_digest.size());
  record_preimage.append(prefix).append(payload_bytes).append(after_record_digest);
  if (result.semantic_content_digest !=
          content_identity::framed_sha256_of_canonical_json(
              kSourceSnapshotSemanticIdentityDomain, kSourceSnapshotApiVersion,
              semantic_preimage) ||
      result.record_digest !=
          content_identity::framed_sha256_of_canonical_json(
              kSourceSnapshotRecordIdentityDomain, kSourceSnapshotApiVersion,
              record_preimage)) {
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
  return ReadObservedAt(directory.get(), file_name_, last_verified_,
                        decode_count_);
}

std::uint64_t AtomicSourceSnapshotChannel::decode_count() const {
  std::lock_guard lock(mutex_);
  return decode_count_;
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
  // The current file is read on every publication. Bytes identical to this
  // channel's own last publication reuse that verified value; any other
  // bytes (a foreign writer, corruption, or another channel) are decoded and
  // verified completely before the generation decision.
  const auto current = ReadObservedAt(directory.get(), file_name_,
                                      last_verified_, decode_count_);
  auto semantic_digest = SemanticDigest(
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

  auto next = CreateRecord(std::move(source_owner_kind),
                           std::move(source_owner_id),
                           std::move(source_owner_epoch), generation,
                           observed_at_ns, std::move(payload),
                           std::move(semantic_digest));
  auto bytes = EncodeCreated(next);
  last_verified_.reset();
  PublishAt(directory.get(), file_name_, bytes);
  last_verified_.emplace(VerifiedRecord{std::move(bytes), next});
  return next;
}

} // namespace manyforge::headless
