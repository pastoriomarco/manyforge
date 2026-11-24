#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace manyforge::collision {

/// Per-file byte bound for one Program-authored scene mesh captured into the
/// activation raw-artifact store. It is the retained planning-model per-resource
/// bound (RetainedPlanningModelStage::kHardMaximumResourceBytes); scene meshes
/// are not admitted above what a retained model resource may carry.
inline constexpr std::uint64_t kMaximumSceneMeshBytes =
    256ULL * 1024ULL * 1024ULL;

/// Path of one captured scene mesh below the activation (content) root:
/// `raw-artifacts/sha256/<64 hex>.blob`, the raw-artifact store layout. Empty
/// when `content_digest` is not an exact `sha256:<64 lowercase hex>` digest.
[[nodiscard]] std::string scene_mesh_store_path(
    std::string_view content_digest);

/// True only for a plain relative resource path: no URI scheme, not absolute,
/// lexically normal, and no empty, `.` or `..` segment.
[[nodiscard]] bool is_plain_relative_resource_path(
    std::string_view path) noexcept;

/// One regular scene-mesh file opened below the scene content root.
///
/// The object owns the validated file descriptor. Loaders read the file only
/// through loader_path() (`/proc/self/fd/<fd>`) while this object is alive, so
/// the bytes are never re-resolved by name. content_digest() is the exact
/// `sha256:<64 lowercase hex>` of the bytes read through that descriptor.
class OpenedSceneMesh final {
 public:
  /// Walks `relative_uri` below the directory descriptor `root_fd` with one
  /// openat(O_NOFOLLOW) per segment, so neither a symlinked directory nor a
  /// symlinked leaf is followed. Requires a regular file of at most
  /// kMaximumSceneMeshBytes. The root is never resolved by path: renaming or
  /// replacing the directory the descriptor was opened from does not change
  /// what is read. Returns nullopt and sets `error` on any refusal.
  [[nodiscard]] static std::optional<OpenedSceneMesh> open(
      int root_fd, std::string_view relative_uri, std::string& error);

  OpenedSceneMesh(OpenedSceneMesh&& other) noexcept;
  OpenedSceneMesh& operator=(OpenedSceneMesh&& other) noexcept;
  OpenedSceneMesh(const OpenedSceneMesh&) = delete;
  OpenedSceneMesh& operator=(const OpenedSceneMesh&) = delete;
  ~OpenedSceneMesh();

  [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
  [[nodiscard]] const std::string& content_digest() const noexcept {
    return content_digest_;
  }
  [[nodiscard]] std::filesystem::path loader_path() const;

 private:
  OpenedSceneMesh(int descriptor, std::string content_digest) noexcept;

  int descriptor_{-1};
  std::string content_digest_;
};

}  // namespace manyforge::collision
