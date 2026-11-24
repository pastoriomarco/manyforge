#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace manyforge::collision {

/// Topology of one triangle mesh, as a mesh loader gives it: vertex
/// positions and triangles indexing them. Coal-free, so the engine's mesh
/// preparation and any other reader of the same mesh share one check.
struct MeshTopology final {
  static constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

  /// Every edge is shared by exactly two triangles that traverse it in
  /// opposite directions, and the enclosed volume is not zero: the surface is
  /// watertight and consistently wound.
  bool closed = false;
  /// Closed with a negative signed volume (exported inside-out); `triangles`
  /// are already flipped.
  bool inside_out = false;
  /// Per input vertex, the lowest index of a vertex at exactly the same
  /// position (welded index).
  std::vector<std::uint32_t> welded;
  /// The input triangles over welded indices, in input order, wound outward
  /// when `closed`. A triangle that repeats a welded index has no surface; it
  /// is `degenerate` and takes no part in the checks.
  std::vector<std::array<std::uint32_t, 3>> triangles;
  std::vector<bool> degenerate;
  /// When `closed`: the triangle across edge k (triangles[t][k] ->
  /// triangles[t][(k + 1) % 3]) of triangle t; kNone for a degenerate one.
  std::vector<std::array<std::uint32_t, 3>> neighbors;
};

/// The closed-surface and orientation checks of one mesh (OD-363).
[[nodiscard]] MeshTopology check_mesh_topology(
    std::span<const std::array<double, 3>> vertices,
    std::span<const std::array<std::uint32_t, 3>> triangles);

}  // namespace manyforge::collision
