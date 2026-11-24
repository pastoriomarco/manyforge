#pragma once

#include <Eigen/Core>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <coal/BVH/BVH_model.h>

namespace manyforge::collision::detail {

/// Vertices visited per pair and direction before a depth query gives up
/// (OD-363). A count, never a time: a slower machine cannot reach it.
inline constexpr std::size_t kMeshDepthVertexCap = 256;

/// One triangle mesh prepared for inside-vertex depth (OD-363): its closed
/// and orientation checks, welded outward-wound triangles, vertex adjacency
/// and the angle-weighted pseudo-normals of its faces, edges and vertices
/// (Baerentzen and Aanaes 2005). Built once where the mesh is loaded and
/// shared like the mesh itself. Only coal's OBBRSS meshes (what coal's and
/// Pinocchio's mesh loaders build) are prepared.
struct MeshDepthShape final {
  std::shared_ptr<const coal::BVHModel<coal::OBBRSS>> bvh;
  bool closed = false;
  /// The rest is filled only when `closed`. Indexed like the mesh's own
  /// vertices and triangles; a vertex index is its welded index.
  std::vector<Eigen::Vector3d> vertices;
  std::vector<std::array<std::uint32_t, 3>> triangles;
  std::vector<bool> degenerate;
  std::vector<Eigen::Vector3d> face_normals;
  /// Per triangle, the pseudo-normal of edge k (corner k to corner k + 1).
  std::vector<std::array<Eigen::Vector3d, 3>> edge_normals;
  std::vector<Eigen::Vector3d> vertex_normals;
  /// Sorted neighbours of welded vertex v: adjacency[offsets[v] .. offsets[v + 1]).
  std::vector<std::uint32_t> adjacency_offsets;
  std::vector<std::uint32_t> adjacency;
  /// One vertex of the surface, for the fully-inside test.
  std::uint32_t anchor_vertex = 0;
};

/// Null when `geometry` is not an OBBRSS triangle mesh.
[[nodiscard]] std::shared_ptr<const MeshDepthShape> prepare_mesh_depth(
    const std::shared_ptr<const coal::CollisionGeometry>& geometry);

struct MeshPose final {
  Eigen::Matrix3d rotation = Eigen::Matrix3d::Identity();
  Eigen::Vector3d translation = Eigen::Vector3d::Zero();
};

enum class MeshDepthStatus : std::uint8_t {
  kOk,
  kMeshNotClosed,
  kVertexCapReached,
};

struct MeshDepthResult final {
  MeshDepthStatus status = MeshDepthStatus::kOk;
  /// A vertex of one mesh lies inside the other.
  bool found = false;
  /// The largest distance of such a vertex from the other surface.
  double depth_m = 0.0;
  /// Unit, world frame, from `first` toward `second`: moving `second` along
  /// it by `depth_m` separates them (the pseudo-normal at the deepest
  /// vertex's closest point, out of the body it is inside).
  Eigen::Vector3d normal_first_to_second = Eigen::Vector3d::Zero();
};

inline constexpr std::uint32_t kNoSeedTriangle = 0xffffffffU;

/// Depth of two colliding meshes from their inside vertices (OD-363): seeds
/// from the colliding triangles (`*_seed_triangle`, kNoSeedTriangle when
/// none), each inside vertex spreads to its neighbours, both directions; when
/// neither finds an inside vertex, one vertex of each body is tested against
/// the other (fully inside). Fixed visit order, no state between queries,
/// no allocation.
[[nodiscard]] MeshDepthResult mesh_depth(const MeshDepthShape& first,
                                         const MeshPose& first_pose,
                                         std::uint32_t first_seed_triangle,
                                         const MeshDepthShape& second,
                                         const MeshPose& second_pose,
                                         std::uint32_t second_seed_triangle) noexcept;

}  // namespace manyforge::collision::detail
