#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <manyforge_core/types/strong_id.hpp>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace manyforge::deployment {

// The byte ceiling is the primary durable-provenance bound. The count ceiling
// remains finite but permits long-lived repositories to retain many historical
// three-manifest build closures without an unsafe implicit eviction policy.
inline constexpr std::size_t kMaxQualifiedManifestCount = 4096U;
inline constexpr std::uint64_t kMaxQualifiedManifestBytes =
    64ULL * 1024ULL * 1024ULL;

class BuildManifestError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

enum class BuildManifestKind : std::uint8_t {
  kNodeEnvironment = 0,
  kCompilerContract,
  kCompilerBuild,
};

[[nodiscard]] constexpr std::string_view
to_string(BuildManifestKind kind) noexcept {
  switch (kind) {
    case BuildManifestKind::kNodeEnvironment:
      return "node_environment";
    case BuildManifestKind::kCompilerContract:
      return "compiler_contract";
    case BuildManifestKind::kCompilerBuild:
      return "compiler_build";
  }
  return {};
}

struct BuildManifestKey final {
  BuildManifestKind manifest_kind;
  core::ContentDigest content_digest;

  friend bool operator==(const BuildManifestKey&,
                         const BuildManifestKey&) = default;
  friend auto operator<=>(const BuildManifestKey&,
                          const BuildManifestKey&) = default;
};

namespace detail {
class BuildQualifiedRetentionAuthority;
class BuildManifestTestAccess;
}

/// One immutable, independently shape/identity-verified semantic manifest.
///
/// Construction is sealed. Callers can retain and retrieve qualified values,
/// but cannot turn arbitrary canonical bytes or digest strings into authority.
/// A retrieved value may describe a historical build; only the internal
/// build-owned retention context proves equality with the current executable.
class BuildQualifiedManifest final {
 public:
  [[nodiscard]] const BuildManifestKey& key() const noexcept { return key_; }
  [[nodiscard]] std::string_view canonical_bytes() const noexcept {
    return canonical_bytes_;
  }

 private:
  BuildQualifiedManifest(BuildManifestKey key, std::string canonical_bytes)
      : key_(std::move(key)),
        canonical_bytes_(std::move(canonical_bytes)) {}

  BuildManifestKey key_;
  std::string canonical_bytes_;

  friend class detail::BuildQualifiedRetentionAuthority;
  friend class detail::BuildManifestTestAccess;
};

/// Semantic build-manifest repository.
///
/// The key is the strong `(manifest kind, content digest)` pair. There is no
/// mutable current/active alias. Retrieval reparses and requalifies stored
/// canonical bytes instead of returning a previously trusted object.
class IBuildManifestRepository {
 public:
  virtual ~IBuildManifestRepository() = default;

  [[nodiscard]] virtual bool put_if_absent(
      std::shared_ptr<const BuildQualifiedManifest> manifest) = 0;
  [[nodiscard]] virtual std::shared_ptr<const BuildQualifiedManifest>
  get_verified(const BuildManifestKey& key) const = 0;
};

class InMemoryBuildManifestRepository final
    : public IBuildManifestRepository {
 public:
  explicit InMemoryBuildManifestRepository(
      std::size_t max_manifest_count = kMaxQualifiedManifestCount,
      std::uint64_t max_total_bytes = kMaxQualifiedManifestBytes);
  ~InMemoryBuildManifestRepository() override;

  InMemoryBuildManifestRepository(
      const InMemoryBuildManifestRepository&) = delete;
  InMemoryBuildManifestRepository& operator=(
      const InMemoryBuildManifestRepository&) = delete;
  InMemoryBuildManifestRepository(
      InMemoryBuildManifestRepository&&) = delete;
  InMemoryBuildManifestRepository& operator=(
      InMemoryBuildManifestRepository&&) = delete;

  [[nodiscard]] bool put_if_absent(
      std::shared_ptr<const BuildQualifiedManifest> manifest) override;
  [[nodiscard]] std::shared_ptr<const BuildQualifiedManifest>
  get_verified(const BuildManifestKey& key) const override;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::uint64_t retained_bytes() const noexcept;

 private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
};

/// Durable immutable semantic-manifest repository.
///
/// Each qualified manifest is stored under its strong kind/digest key using a
/// same-directory write/fsync/link publication sequence. A fresh repository
/// process independently reparses and requalifies bytes before returning
/// them. There is no mutable current/active alias.
///
/// The configured root and its ancestors are trusted deployment-owned
/// storage: they must not be renamed or replaced concurrently by an
/// untrusted actor. Within that stable root, entries are required to be plain
/// files/directories and publication is immutable and process-serialized.
class FilesystemBuildManifestRepository final
    : public IBuildManifestRepository {
 public:
  explicit FilesystemBuildManifestRepository(
      std::filesystem::path root,
      std::size_t max_manifest_count = kMaxQualifiedManifestCount,
      std::uint64_t max_total_bytes = kMaxQualifiedManifestBytes);
  ~FilesystemBuildManifestRepository() override;

  FilesystemBuildManifestRepository(
      const FilesystemBuildManifestRepository&) = delete;
  FilesystemBuildManifestRepository& operator=(
      const FilesystemBuildManifestRepository&) = delete;
  FilesystemBuildManifestRepository(
      FilesystemBuildManifestRepository&&) = delete;
  FilesystemBuildManifestRepository& operator=(
      FilesystemBuildManifestRepository&&) = delete;

  [[nodiscard]] bool put_if_absent(
      std::shared_ptr<const BuildQualifiedManifest> manifest) override;
  [[nodiscard]] std::shared_ptr<const BuildQualifiedManifest>
  get_verified(const BuildManifestKey& key) const override;

  /// Returns a repository-wide snapshot refreshed under the cross-process
  /// repository lock.
  [[nodiscard]] std::size_t size() const;
  /// Returns a repository-wide snapshot refreshed under the cross-process
  /// repository lock.
  [[nodiscard]] std::uint64_t retained_bytes() const;

 private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
};

}  // namespace manyforge::deployment
