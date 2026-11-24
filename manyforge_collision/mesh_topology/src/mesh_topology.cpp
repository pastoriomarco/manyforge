#include <manyforge_collision/mesh_topology.hpp>

#include <algorithm>
#include <numeric>
#include <tuple>

namespace manyforge::collision {

MeshTopology check_mesh_topology(
    const std::span<const std::array<double, 3>> vertices,
    const std::span<const std::array<std::uint32_t, 3>> triangles) {
  MeshTopology topology;
  const auto vertex_count = static_cast<std::uint32_t>(vertices.size());

  // Weld exactly coincident vertices: loaders may repeat a position per
  // triangle (STL) or per sub-mesh.
  std::vector<std::uint32_t> order(vertices.size());
  std::iota(order.begin(), order.end(), 0U);
  std::sort(order.begin(), order.end(), [&](std::uint32_t lhs, std::uint32_t rhs) {
    return std::tie(vertices[lhs], lhs) < std::tie(vertices[rhs], rhs);
  });
  topology.welded.resize(vertices.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    topology.welded[order[i]] = i > 0 && vertices[order[i]] == vertices[order[i - 1]]
                                    ? topology.welded[order[i - 1]]
                                    : order[i];
  }

  topology.triangles.reserve(triangles.size());
  topology.degenerate.reserve(triangles.size());
  for (const auto& triangle : triangles) {
    if (triangle[0] >= vertex_count || triangle[1] >= vertex_count ||
        triangle[2] >= vertex_count) {
      return topology;  // Not a mesh over these vertices: not closed.
    }
    const std::array<std::uint32_t, 3> welded{topology.welded[triangle[0]],
                                              topology.welded[triangle[1]],
                                              topology.welded[triangle[2]]};
    topology.triangles.push_back(welded);
    topology.degenerate.push_back(welded[0] == welded[1] || welded[1] == welded[2] ||
                                  welded[2] == welded[0]);
  }

  // Directed edges (from, to, triangle, edge index): closed when each one is
  // unique and its reverse exists.
  struct DirectedEdge {
    std::uint32_t from, to, triangle, edge;
  };
  std::vector<DirectedEdge> edges;
  edges.reserve(3U * topology.triangles.size());
  for (std::uint32_t t = 0; t < topology.triangles.size(); ++t) {
    if (topology.degenerate[t]) continue;
    for (std::uint32_t k = 0; k < 3U; ++k) {
      edges.push_back({topology.triangles[t][k], topology.triangles[t][(k + 1U) % 3U], t, k});
    }
  }
  if (edges.empty()) return topology;
  const auto key_less = [](const DirectedEdge& lhs, const DirectedEdge& rhs) {
    return std::tie(lhs.from, lhs.to) < std::tie(rhs.from, rhs.to);
  };
  std::sort(edges.begin(), edges.end(), key_less);
  topology.neighbors.assign(topology.triangles.size(),
                            {MeshTopology::kNone, MeshTopology::kNone, MeshTopology::kNone});
  for (std::size_t i = 0; i < edges.size(); ++i) {
    const auto& edge = edges[i];
    if (i + 1U < edges.size() && edges[i + 1U].from == edge.from && edges[i + 1U].to == edge.to) {
      topology.neighbors.clear();
      return topology;  // Two triangles run the same way along one edge.
    }
    const DirectedEdge reverse{edge.to, edge.from, 0U, 0U};
    const auto found = std::lower_bound(edges.begin(), edges.end(), reverse, key_less);
    if (found == edges.end() || found->from != edge.to || found->to != edge.from) {
      topology.neighbors.clear();
      return topology;  // A border edge: the surface is open.
    }
    topology.neighbors[edge.triangle][edge.edge] = found->triangle;
  }

  // Six times the signed volume enclosed by the outward-wound surface.
  double volume = 0.0;
  for (std::uint32_t t = 0; t < topology.triangles.size(); ++t) {
    if (topology.degenerate[t]) continue;
    const auto& a = vertices[topology.triangles[t][0]];
    const auto& b = vertices[topology.triangles[t][1]];
    const auto& c = vertices[topology.triangles[t][2]];
    volume += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
              a[2] * (b[0] * c[1] - b[1] * c[0]);
  }
  if (volume == 0.0) {
    topology.neighbors.clear();
    return topology;
  }
  topology.closed = true;
  topology.inside_out = volume < 0.0;
  if (topology.inside_out) {
    // Reverse every triangle (swap corners 1 and 2). Old edges: 0 = (0,1),
    // 1 = (1,2), 2 = (2,0); new edge 0 = (0,2) is old 2, new 1 = (2,1) is
    // old 1, new 2 = (1,0) is old 0: the neighbors across them swap alike.
    for (std::uint32_t t = 0; t < topology.triangles.size(); ++t) {
      std::swap(topology.triangles[t][1], topology.triangles[t][2]);
      std::swap(topology.neighbors[t][0], topology.neighbors[t][2]);
    }
  }
  return topology;
}

}  // namespace manyforge::collision
