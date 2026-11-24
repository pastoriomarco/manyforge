#pragma once

#include <array>
#include <optional>
#include <string_view>

#include <manyforge_collision/mesh_topology.hpp>

namespace manyforge::collision {

/// The topology of the mesh file `bytes` as the collision engine loads it:
/// read by assimp with the post-processing of coal's mesh loader, every
/// node's meshes under its transform, scaled by `scale`. `format_hint` is the
/// file extension without the dot (for example "stl"). Nullopt when assimp
/// cannot read the bytes.
[[nodiscard]] std::optional<MeshTopology> read_mesh_topology(
    std::string_view bytes, std::string_view format_hint,
    const std::array<double, 3>& scale);

}  // namespace manyforge::collision
