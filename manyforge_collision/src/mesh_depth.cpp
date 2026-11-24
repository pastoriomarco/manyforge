#include <manyforge_collision/mesh_depth.hpp>

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <manyforge_collision/mesh_topology.hpp>

namespace manyforge::collision::detail {
namespace {

constexpr std::size_t kCap = kMeshDepthVertexCap;

/// Closest-point region on a triangle: its face, edge k (corner k to
/// k + 1) or corner k.
enum class Region : std::uint8_t { kFace, kEdge0, kEdge1, kEdge2, kVertex0, kVertex1, kVertex2 };

struct ClosestOnTriangle {
  Eigen::Vector3d point;
  Region region;
};

/// Ericson, Real-Time Collision Detection, 5.1.5, with the region it ends in.
ClosestOnTriangle closest_on_triangle(const Eigen::Vector3d& p, const Eigen::Vector3d& a,
                                      const Eigen::Vector3d& b,
                                      const Eigen::Vector3d& c) noexcept {
  const Eigen::Vector3d ab = b - a;
  const Eigen::Vector3d ac = c - a;
  const Eigen::Vector3d ap = p - a;
  const double d1 = ab.dot(ap);
  const double d2 = ac.dot(ap);
  if (d1 <= 0.0 && d2 <= 0.0) return {a, Region::kVertex0};
  const Eigen::Vector3d bp = p - b;
  const double d3 = ab.dot(bp);
  const double d4 = ac.dot(bp);
  if (d3 >= 0.0 && d4 <= d3) return {b, Region::kVertex1};
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
    return {a + (d1 / (d1 - d3)) * ab, Region::kEdge0};
  }
  const Eigen::Vector3d cp = p - c;
  const double d5 = ab.dot(cp);
  const double d6 = ac.dot(cp);
  if (d6 >= 0.0 && d5 <= d6) return {c, Region::kVertex2};
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
    return {a + (d2 / (d2 - d6)) * ac, Region::kEdge2};
  }
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
    return {b + ((d4 - d3) / ((d4 - d3) + (d5 - d6))) * (c - b), Region::kEdge1};
  }
  const double denominator = 1.0 / (va + vb + vc);
  return {a + ab * (vb * denominator) + ac * (vc * denominator), Region::kFace};
}

/// Squared distance from `p` to an OBB: a lower bound for every triangle in
/// the node.
double obb_distance_squared(const coal::OBB& obb, const Eigen::Vector3d& p) noexcept {
  const Eigen::Vector3d d = p - obb.To;
  double sum = 0.0;
  for (int k = 0; k < 3; ++k) {
    const double excess = std::abs(obb.axes.col(k).dot(d)) - obb.extent[k];
    if (excess > 0.0) sum += excess * excess;
  }
  return sum;
}

struct ClosestOnMesh {
  double distance_squared = std::numeric_limits<double>::infinity();
  Eigen::Vector3d point = Eigen::Vector3d::Zero();
  Eigen::Vector3d normal = Eigen::Vector3d::Zero();
};

void closest_on_mesh(const MeshDepthShape& shape, const Eigen::Vector3d& p, int node_index,
                     double node_bound, ClosestOnMesh& best) noexcept {
  if (node_bound >= best.distance_squared) return;
  const auto& node = shape.bvh->getBV(static_cast<unsigned int>(node_index));
  if (node.isLeaf()) {
    const auto t = static_cast<std::uint32_t>(node.primitiveId());
    if (t >= shape.triangles.size() || shape.degenerate[t]) return;
    const auto& triangle = shape.triangles[t];
    const auto closest = closest_on_triangle(p, shape.vertices[triangle[0]],
                                             shape.vertices[triangle[1]],
                                             shape.vertices[triangle[2]]);
    const double distance_squared = (p - closest.point).squaredNorm();
    if (!(distance_squared < best.distance_squared)) return;
    best.distance_squared = distance_squared;
    best.point = closest.point;
    switch (closest.region) {
      case Region::kFace:
        best.normal = shape.face_normals[t];
        break;
      case Region::kEdge0:
      case Region::kEdge1:
      case Region::kEdge2:
        best.normal = shape.edge_normals[t][static_cast<std::size_t>(closest.region) -
                                            static_cast<std::size_t>(Region::kEdge0)];
        break;
      case Region::kVertex0:
      case Region::kVertex1:
      case Region::kVertex2:
        best.normal = shape.vertex_normals[triangle[static_cast<std::size_t>(closest.region) -
                                                    static_cast<std::size_t>(Region::kVertex0)]];
        break;
    }
    return;
  }
  const int left = node.leftChild();
  const int right = node.rightChild();
  const double left_bound =
      obb_distance_squared(shape.bvh->getBV(static_cast<unsigned int>(left)).bv.obb, p);
  const double right_bound =
      obb_distance_squared(shape.bvh->getBV(static_cast<unsigned int>(right)).bv.obb, p);
  // Nearer child first; the left one on a tie, so the order is fixed.
  if (right_bound < left_bound) {
    closest_on_mesh(shape, p, right, right_bound, best);
    closest_on_mesh(shape, p, left, left_bound, best);
  } else {
    closest_on_mesh(shape, p, left, left_bound, best);
    closest_on_mesh(shape, p, right, right_bound, best);
  }
}

/// The vertices of `moving` inside `fixed`, from seeds through adjacency.
class Pass final {
 public:
  Pass(const MeshDepthShape& moving, const MeshPose& moving_pose, const MeshDepthShape& fixed,
       const MeshPose& fixed_pose) noexcept
      : moving_(moving),
        fixed_(fixed),
        rotation_(fixed_pose.rotation.transpose() * moving_pose.rotation),
        translation_(fixed_pose.rotation.transpose() *
                     (moving_pose.translation - fixed_pose.translation)) {}

  /// False when the cap is reached.
  [[nodiscard]] bool seed_triangle(std::uint32_t triangle) noexcept {
    if (triangle >= moving_.triangles.size()) return true;
    for (const auto vertex : moving_.triangles[triangle]) {
      if (!classify(vertex)) return false;
    }
    return spread();
  }

  [[nodiscard]] bool seed_vertex(std::uint32_t vertex) noexcept {
    return classify(vertex) && spread();
  }

  [[nodiscard]] bool found() const noexcept { return found_; }
  [[nodiscard]] double depth() const noexcept { return depth_; }
  /// Out of `fixed`, in its frame.
  [[nodiscard]] const Eigen::Vector3d& normal() const noexcept { return normal_; }

 private:
  bool classify(std::uint32_t vertex) noexcept {
    const auto end = visited_.begin() + static_cast<std::ptrdiff_t>(visited_count_);
    const auto at = std::lower_bound(visited_.begin(), end, vertex);
    if (at != end && *at == vertex) return true;
    if (visited_count_ == kCap) return false;
    std::move_backward(at, end, end + 1);
    *at = vertex;
    ++visited_count_;

    const Eigen::Vector3d p = rotation_ * moving_.vertices[vertex] + translation_;
    ClosestOnMesh closest;
    const auto& root = fixed_.bvh->getBV(0U);
    closest_on_mesh(fixed_, p, 0, obb_distance_squared(root.bv.obb, p), closest);
    const Eigen::Vector3d offset = p - closest.point;
    if (!(offset.dot(closest.normal) < 0.0)) return true;  // Outside or on the surface.
    queue_[queue_tail_++] = vertex;
    const double depth = std::sqrt(closest.distance_squared);
    if (!found_ || depth > depth_) {
      found_ = true;
      depth_ = depth;
      normal_ = closest.normal;
    }
    return true;
  }

  bool spread() noexcept {
    while (queue_head_ < queue_tail_) {
      const auto vertex = queue_[queue_head_++];
      for (auto i = moving_.adjacency_offsets[vertex]; i < moving_.adjacency_offsets[vertex + 1U];
           ++i) {
        if (!classify(moving_.adjacency[i])) return false;
      }
    }
    return true;
  }

  const MeshDepthShape& moving_;
  const MeshDepthShape& fixed_;
  Eigen::Matrix3d rotation_;
  Eigen::Vector3d translation_;
  std::array<std::uint32_t, kCap> visited_{};
  std::size_t visited_count_ = 0;
  std::array<std::uint32_t, kCap> queue_{};
  std::size_t queue_head_ = 0;
  std::size_t queue_tail_ = 0;
  bool found_ = false;
  double depth_ = 0.0;
  Eigen::Vector3d normal_ = Eigen::Vector3d::Zero();
};

}  // namespace

std::shared_ptr<const MeshDepthShape> prepare_mesh_depth(
    const std::shared_ptr<const coal::CollisionGeometry>& geometry) {
  auto bvh = std::dynamic_pointer_cast<const coal::BVHModel<coal::OBBRSS>>(geometry);
  if (!bvh) return nullptr;
  auto shape = std::make_shared<MeshDepthShape>();
  shape->bvh = bvh;
  if (!bvh->vertices || !bvh->tri_indices || bvh->getNumBVs() == 0U) return shape;

  std::vector<std::array<double, 3>> positions;
  positions.reserve(bvh->num_vertices);
  for (unsigned int i = 0; i < bvh->num_vertices; ++i) {
    const auto& v = (*bvh->vertices)[i];
    positions.push_back({static_cast<double>(v[0]), static_cast<double>(v[1]),
                         static_cast<double>(v[2])});
  }
  std::vector<std::array<std::uint32_t, 3>> triangles;
  triangles.reserve(bvh->num_tris);
  for (unsigned int i = 0; i < bvh->num_tris; ++i) {
    const auto& triangle = (*bvh->tri_indices)[i];
    triangles.push_back({static_cast<std::uint32_t>(triangle[0]),
                         static_cast<std::uint32_t>(triangle[1]),
                         static_cast<std::uint32_t>(triangle[2])});
  }
  auto topology = check_mesh_topology(positions, triangles);
  if (!topology.closed) return shape;

  shape->closed = true;
  shape->vertices.reserve(positions.size());
  for (const auto& p : positions) shape->vertices.emplace_back(p[0], p[1], p[2]);
  shape->triangles = std::move(topology.triangles);
  shape->degenerate = std::move(topology.degenerate);
  const auto triangle_count = shape->triangles.size();
  const auto vertex_count = shape->vertices.size();

  shape->face_normals.assign(triangle_count, Eigen::Vector3d::Zero());
  for (std::size_t t = 0; t < triangle_count; ++t) {
    if (shape->degenerate[t]) continue;
    const auto& triangle = shape->triangles[t];
    const Eigen::Vector3d normal =
        (shape->vertices[triangle[1]] - shape->vertices[triangle[0]])
            .cross(shape->vertices[triangle[2]] - shape->vertices[triangle[0]]);
    const double norm = normal.norm();
    if (norm > 0.0) {
      shape->face_normals[t] = normal / norm;
    } else {
      shape->degenerate[t] = true;  // Zero area: no closest point of its own.
    }
  }

  // Edge pseudo-normal: the sum of the two faces' normals.
  shape->edge_normals.resize(triangle_count);
  for (std::size_t t = 0; t < triangle_count; ++t) {
    for (std::size_t k = 0; k < 3U; ++k) {
      const auto neighbor = topology.neighbors[t][k];
      Eigen::Vector3d normal = shape->face_normals[t];
      if (neighbor != MeshTopology::kNone) normal += shape->face_normals[neighbor];
      const double norm = normal.norm();
      shape->edge_normals[t][k] = norm > 0.0 ? Eigen::Vector3d(normal / norm)
                                             : shape->face_normals[t];
    }
  }

  // Vertex pseudo-normal: face normals weighted by the angle at the vertex.
  shape->vertex_normals.assign(vertex_count, Eigen::Vector3d::Zero());
  std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
  edges.reserve(6U * triangle_count);
  bool anchored = false;
  for (std::size_t t = 0; t < triangle_count; ++t) {
    const auto& triangle = shape->triangles[t];
    for (std::size_t k = 0; k < 3U; ++k) {
      const auto vertex = triangle[k];
      const auto next = triangle[(k + 1U) % 3U];
      const auto previous = triangle[(k + 2U) % 3U];
      if (vertex != next) {
        edges.emplace_back(vertex, next);
        edges.emplace_back(next, vertex);
      }
      if (shape->degenerate[t]) continue;
      const Eigen::Vector3d u = shape->vertices[next] - shape->vertices[vertex];
      const Eigen::Vector3d w = shape->vertices[previous] - shape->vertices[vertex];
      const double angle = std::atan2(u.cross(w).norm(), u.dot(w));
      shape->vertex_normals[vertex] += angle * shape->face_normals[t];
    }
    if (!anchored && !shape->degenerate[t]) {
      shape->anchor_vertex = triangle[0];
      anchored = true;
    }
  }
  for (auto& normal : shape->vertex_normals) {
    const double norm = normal.norm();
    if (norm > 0.0) normal /= norm;
  }

  // Adjacency over welded vertices, each list sorted: the fixed visit order.
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  shape->adjacency_offsets.assign(vertex_count + 1U, 0U);
  for (const auto& edge : edges) ++shape->adjacency_offsets[edge.first + 1U];
  for (std::size_t v = 0; v < vertex_count; ++v) {
    shape->adjacency_offsets[v + 1U] += shape->adjacency_offsets[v];
  }
  shape->adjacency.reserve(edges.size());
  for (const auto& edge : edges) shape->adjacency.push_back(edge.second);
  return shape;
}

MeshDepthResult mesh_depth(const MeshDepthShape& first, const MeshPose& first_pose,
                           const std::uint32_t first_seed_triangle,
                           const MeshDepthShape& second, const MeshPose& second_pose,
                           const std::uint32_t second_seed_triangle) noexcept {
  MeshDepthResult result;
  if (!first.closed || !second.closed) {
    result.status = MeshDepthStatus::kMeshNotClosed;
    return result;
  }
  // Vertices of `second` inside `first`, then the reverse pass.
  Pass into_first(second, second_pose, first, first_pose);
  Pass into_second(first, first_pose, second, second_pose);
  bool within_cap = into_first.seed_triangle(second_seed_triangle) &&
                    into_second.seed_triangle(first_seed_triangle);
  if (within_cap && !into_first.found() && !into_second.found()) {
    // Fully inside: no colliding triangle has a vertex inside the other body.
    within_cap = into_first.seed_vertex(second.anchor_vertex) &&
                 into_second.seed_vertex(first.anchor_vertex);
  }
  if (!within_cap) {
    result.status = MeshDepthStatus::kVertexCapReached;
    return result;
  }
  if (into_first.found() && (!into_second.found() || into_first.depth() >= into_second.depth())) {
    result.found = true;
    result.depth_m = into_first.depth();
    // Out of `first`: moving `second` along it separates them.
    result.normal_first_to_second = (first_pose.rotation * into_first.normal()).normalized();
  } else if (into_second.found()) {
    result.found = true;
    result.depth_m = into_second.depth();
    result.normal_first_to_second = -(second_pose.rotation * into_second.normal()).normalized();
  }
  return result;
}

}  // namespace manyforge::collision::detail
