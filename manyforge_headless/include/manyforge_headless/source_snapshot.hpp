#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <manyforge_content_identity/canonical_value.hpp>

namespace manyforge::headless {

inline constexpr std::string_view kSourceSnapshotApiVersion =
    "manyforge.source_snapshot.v1";
inline constexpr std::string_view kSourceSnapshotSemanticIdentityDomain =
    "manyforge-source-snapshot-semantic";
inline constexpr std::string_view kSourceSnapshotRecordIdentityDomain =
    "manyforge-source-snapshot-record";
inline constexpr std::size_t kMaximumSourceSnapshotBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaximumSourceOwnerIdentifierBytes = 256U;

class SourceSnapshotError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class SourceSnapshotIntegrityError final : public SourceSnapshotError {
public:
  using SourceSnapshotError::SourceSnapshotError;
};

class SourceSnapshotIoError final : public SourceSnapshotError {
public:
  using SourceSnapshotError::SourceSnapshotError;
};

/// One immutable source-owner publication consumed by the independently
/// restartable headless projection. Source generation changes only when the
/// semantic content changes inside one source-owner epoch; observation time
/// may advance without fabricating a semantic transition.
struct SourceSnapshot final {
  std::string source_owner_kind;
  std::string source_owner_id;
  std::string source_owner_epoch;
  std::uint64_t source_generation;
  std::int64_t observed_at_ns;
  manyforge::content_identity::CanonicalValue payload;
  std::string semantic_content_digest;
  std::string record_digest;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const SourceSnapshot &,
                         const SourceSnapshot &) = default;
};

/// Receiver-side filesystem observation of one immutable publication. The
/// wall-clock timestamp comes from the exact opened inode and is not included
/// in source-authored identity. It lets a restarted headless observer measure
/// staleness without treating an old file as freshly received.
struct SourceSnapshotFileObservation final {
  SourceSnapshot snapshot;
  std::int64_t file_modified_at_unix_ns;
};

/// Builds and verifies the closed canonical v1 source-publication record.
class SourceSnapshotCodec final {
public:
  [[nodiscard]] static SourceSnapshot
  Create(std::string source_owner_kind, std::string source_owner_id,
         std::string source_owner_epoch, std::uint64_t source_generation,
         std::int64_t observed_at_ns,
         manyforge::content_identity::CanonicalValue payload);

  [[nodiscard]] static std::string Encode(const SourceSnapshot &snapshot);
  [[nodiscard]] static SourceSnapshot Decode(std::string_view canonical_bytes);
};

/// Atomic one-file publication channel for a single semantic source owner.
///
/// Publication uses same-directory temporary write, file fsync, atomic rename,
/// and directory fsync. Readers open the selected inode with O_NOFOLLOW and
/// validate the complete closed record. The channel owns no source semantics;
/// it advances generation by comparing verified semantic digests only.
///
/// Each read still opens, stats and reads the complete selected file. Only
/// when those bytes are identical to the last record this channel completely
/// verified (decoded, or encoded itself) is that verified value reused instead
/// of decoding and re-deriving both digests again; any other byte sequence is
/// fully decoded and verified.
class AtomicSourceSnapshotChannel final {
public:
  AtomicSourceSnapshotChannel(std::filesystem::path directory,
                              std::string file_name);

  [[nodiscard]] const std::filesystem::path &directory() const noexcept;
  [[nodiscard]] const std::string &file_name() const noexcept;

  [[nodiscard]] std::optional<SourceSnapshot> Read() const;
  [[nodiscard]] std::optional<SourceSnapshotFileObservation>
  ReadObserved() const;

  [[nodiscard]] SourceSnapshot
  Publish(std::string source_owner_kind, std::string source_owner_id,
          std::string source_owner_epoch, std::int64_t observed_at_ns,
          manyforge::content_identity::CanonicalValue payload);

  /// Number of complete record decodes performed by this channel's reads (a
  /// diagnostic for byte-identical reuse; reuse does not advance it).
  [[nodiscard]] std::uint64_t decode_count() const;

  /// Exact canonical bytes and value of the last completely verified record.
  struct VerifiedRecord final {
    std::string bytes;
    SourceSnapshot snapshot;
  };

private:
  std::filesystem::path directory_;
  std::string file_name_;
  mutable std::mutex mutex_;
  mutable std::optional<VerifiedRecord> last_verified_;
  mutable std::uint64_t decode_count_{};
};

} // namespace manyforge::headless
