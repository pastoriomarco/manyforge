// Depth between two meshes from inside vertices (OD-363).

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <coal/BVH/BVH_model.h>
#include <coal/collision.h>
#include <coal/distance.h>
#include <coal/shape/geometric_shapes.h>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_collision/mesh_depth.hpp>
#ifdef MANYFORGE_MESH_FILE_TOPOLOGY
#include <manyforge_collision/mesh_file_topology.hpp>
#endif
#include <manyforge_collision/mesh_topology.hpp>
#include <manyforge_collision/scene_asset_root.hpp>
#include <manyforge_content_identity/content_identity.hpp>

namespace {

namespace detail = manyforge::collision::detail;
using manyforge::collision::check_mesh_topology;
using Mesh = coal::BVHModel<coal::OBBRSS>;

struct Triangles {
  std::vector<coal::Vec3s> vertices;
  std::vector<coal::Triangle> triangles;

  void add_triangle(const coal::Vec3s& a, const coal::Vec3s& b, const coal::Vec3s& c) {
    const auto base = vertices.size();
    vertices.insert(vertices.end(), {a, b, c});
    triangles.emplace_back(base, base + 1U, base + 2U);
  }
  /// Counter-clockwise seen from outside.
  void add_quad(const coal::Vec3s& a, const coal::Vec3s& b, const coal::Vec3s& c,
                const coal::Vec3s& d) {
    add_triangle(a, b, c);
    add_triangle(a, c, d);
  }
  void flip() {
    for (auto& triangle : triangles) {
      triangle = coal::Triangle(triangle[0], triangle[2], triangle[1]);
    }
  }
  [[nodiscard]] std::shared_ptr<Mesh> mesh() const {
    auto mesh = std::make_shared<Mesh>();
    mesh->beginModel();
    mesh->addSubModel(vertices, triangles);
    mesh->endModel();
    return mesh;
  }
  [[nodiscard]] std::string obj() const {
    std::ostringstream out;
    out.precision(17);
    for (const auto& v : vertices) out << "v " << v[0] << ' ' << v[1] << ' ' << v[2] << '\n';
    for (const auto& t : triangles) {
      out << "f " << t[0] + 1 << ' ' << t[1] + 1 << ' ' << t[2] + 1 << '\n';
    }
    return out.str();
  }
};

/// A box surface, each face an n x n grid of quads.
Triangles box(const coal::Vec3s& half, const coal::Vec3s& center, int n = 1) {
  Triangles out;
  for (int axis = 0; axis < 3; ++axis) {
    for (const double sign : {1.0, -1.0}) {
      int u = (axis + 1) % 3;
      int v = (axis + 2) % 3;
      if (sign < 0.0) std::swap(u, v);  // u x v points out of the face.
      // Grid coordinates hit the box edges exactly, so neighbouring faces
      // share their edge vertices bit for bit (a closed mesh).
      const auto grid = [n](double h, int i) {
        return i == 0 ? -h : i == n ? h : -h + 2.0 * h * i / n;
      };
      const auto point = [&](int i, int j) {
        coal::Vec3s p = center;
        p[axis] += sign * half[axis];
        p[u] += grid(half[u], i);
        p[v] += grid(half[v], j);
        return p;
      };
      for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
          out.add_quad(point(i, j), point(i + 1, j), point(i + 1, j + 1), point(i, j + 1));
        }
      }
    }
  }
  return out;
}

/// A solid cylinder along z from z0 to z1.
Triangles rod(double radius, double z0, double z1, int segments = 32) {
  Triangles out;
  const auto ring = [&](int i, double z) {
    const double angle = 2.0 * std::numbers::pi * (i % segments) / segments;
    return coal::Vec3s(radius * std::cos(angle), radius * std::sin(angle), z);
  };
  for (int i = 0; i < segments; ++i) {
    out.add_quad(ring(i, z0), ring(i + 1, z0), ring(i + 1, z1), ring(i, z1));
    out.add_triangle(coal::Vec3s(0, 0, z1), ring(i, z1), ring(i + 1, z1));
    out.add_triangle(coal::Vec3s(0, 0, z0), ring(i + 1, z0), ring(i, z0));
  }
  return out;
}

/// A square pyramid, apex down at (0, 0, -height), base at z = 0.
Triangles pyramid(double half, double height) {
  Triangles out;
  const coal::Vec3s apex(0, 0, -height);
  const std::vector<coal::Vec3s> corners{
      {-half, -half, 0}, {half, -half, 0}, {half, half, 0}, {-half, half, 0}};
  out.add_quad(corners[0], corners[1], corners[2], corners[3]);
  for (std::size_t i = 0; i < 4; ++i) out.add_triangle(apex, corners[(i + 1) % 4], corners[i]);
  return out;
}

/// The L-shaped prism over (0,0) (2,0) (2,1) (1,1) (1,2) (0,2) times `s`,
/// z from 0 to `height`; its notch x, y in [s, 2s] is a concave recess.
Triangles l_prism(double s, double height) {
  const std::vector<coal::Vec3s> outline{{0, 0, 0},     {2 * s, 0, 0}, {2 * s, s, 0},
                                         {s, s, 0},     {s, 2 * s, 0}, {0, 2 * s, 0}};
  const auto at = [&](std::size_t i, double z) {
    return coal::Vec3s(outline[i][0], outline[i][1], z);
  };
  Triangles out;
  for (std::size_t i = 0; i < outline.size(); ++i) {
    const auto next = (i + 1) % outline.size();
    out.add_quad(at(i, 0), at(next, 0), at(next, height), at(i, height));
  }
  // Caps: two convex quads fanned from corner 0.
  for (const auto& [a, b, c] : std::vector<std::array<std::size_t, 3>>{
           {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 5}}) {
    out.add_triangle(at(a, height), at(b, height), at(c, height));
    out.add_triangle(at(a, 0), at(c, 0), at(b, 0));
  }
  return out;
}

std::shared_ptr<const detail::MeshDepthShape> prepared(const Triangles& triangles) {
  auto shape = detail::prepare_mesh_depth(triangles.mesh());
  EXPECT_NE(shape, nullptr);
  return shape;
}

coal::Transform3s at(const coal::Vec3s& translation) {
  return coal::Transform3s(coal::Matrix3s::Identity(), translation);
}

/// The colliding triangle pair coal finds, as the engine's seeds.
std::array<std::uint32_t, 2> seeds_of(const detail::MeshDepthShape& first,
                                      const coal::Transform3s& first_pose,
                                      const detail::MeshDepthShape& second,
                                      const coal::Transform3s& second_pose) {
  coal::CollisionRequest request;
  coal::CollisionResult collision;
  coal::collide(first.bvh.get(), first_pose, second.bvh.get(), second_pose, request, collision);
  if (collision.numContacts() == 0U) return {detail::kNoSeedTriangle, detail::kNoSeedTriangle};
  return {static_cast<std::uint32_t>(collision.getContact(0).b1),
          static_cast<std::uint32_t>(collision.getContact(0).b2)};
}

detail::MeshPose mesh_pose(const coal::Transform3s& pose) {
  return {pose.getRotation(), pose.getTranslation()};
}

/// The depth exactly as the engine asks for it.
detail::MeshDepthResult depth_of(const detail::MeshDepthShape& first,
                                 const coal::Transform3s& first_pose,
                                 const detail::MeshDepthShape& second,
                                 const coal::Transform3s& second_pose) {
  const auto seeds = seeds_of(first, first_pose, second, second_pose);
  return detail::mesh_depth(first, mesh_pose(first_pose), seeds[0], second,
                            mesh_pose(second_pose), seeds[1]);
}

// A plate 50 mm thick, top at z = 0.025; a rod whose bottom end is 10.8 mm
// into it (a tube pressed into a plate).
constexpr double kPlateTop = 0.025;
constexpr double kOverlap = 0.0108;

Triangles plate(int n = 4) { return box({0.1, 0.1, 0.025}, {0, 0, 0}, n); }
Triangles tube() { return rod(0.01, kPlateTop - kOverlap, kPlateTop - kOverlap + 0.1); }

TEST(MeshTopology, ClosedBoxIsClosedAndOutward) {
  const auto mesh = box({1, 1, 1}, {0, 0, 0}, 2);
  std::vector<std::array<double, 3>> vertices;
  for (const auto& v : mesh.vertices) vertices.push_back({v[0], v[1], v[2]});
  std::vector<std::array<std::uint32_t, 3>> triangles;
  for (const auto& t : mesh.triangles) {
    triangles.push_back({static_cast<std::uint32_t>(t[0]), static_cast<std::uint32_t>(t[1]),
                         static_cast<std::uint32_t>(t[2])});
  }
  const auto topology = check_mesh_topology(vertices, triangles);
  EXPECT_TRUE(topology.closed);
  EXPECT_FALSE(topology.inside_out);

  std::swap(triangles[0][1], triangles[0][2]);  // One triangle wound the wrong way.
  EXPECT_FALSE(check_mesh_topology(vertices, triangles).closed);
  triangles.erase(triangles.begin());  // A hole.
  EXPECT_FALSE(check_mesh_topology(vertices, triangles).closed);
}

#ifdef MANYFORGE_MESH_FILE_TOPOLOGY
// The Manager's Apply preview reads a mesh file the way the engine loads it.
TEST(MeshTopology, MeshFilesAreCheckedAsTheEngineLoadsThem) {
  const auto closed = manyforge::collision::read_mesh_topology(plate().obj(), "obj", {1, 1, 1});
  ASSERT_TRUE(closed.has_value());
  EXPECT_TRUE(closed->closed);
  EXPECT_FALSE(closed->inside_out);

  auto inside_out = plate();
  inside_out.flip();
  const auto flipped =
      manyforge::collision::read_mesh_topology(inside_out.obj(), "obj", {0.001, 0.001, 0.001});
  ASSERT_TRUE(flipped.has_value());
  EXPECT_TRUE(flipped->closed);
  EXPECT_TRUE(flipped->inside_out);

  auto open = plate();
  open.triangles.pop_back();
  const auto holed = manyforge::collision::read_mesh_topology(open.obj(), "obj", {1, 1, 1});
  ASSERT_TRUE(holed.has_value());
  EXPECT_FALSE(holed->closed);
}
#endif

TEST(MeshDepth, TubeIntoPlateReadsTheAxialOverlap) {
  const auto plate_shape = prepared(plate());
  const auto tube_shape = prepared(tube());
  ASSERT_TRUE(plate_shape->closed);
  ASSERT_TRUE(tube_shape->closed);
  const auto depth = depth_of(*plate_shape, at({0, 0, 0}), *tube_shape, at({0, 0, 0}));
  ASSERT_EQ(depth.status, detail::MeshDepthStatus::kOk);
  ASSERT_TRUE(depth.found);
  EXPECT_NEAR(depth.depth_m, kOverlap, 1e-9);
  // Out of the plate: moving the tube up separates them.
  EXPECT_NEAR(depth.normal_first_to_second.z(), 1.0, 1e-9);

  // Coal's own signed distance of the two meshes reads the triangle overlap.
  coal::DistanceRequest request;
  request.enable_signed_distance = true;
  coal::DistanceResult distance;
  coal::distance(plate_shape->bvh.get(), at({0, 0, 0}), tube_shape->bvh.get(), at({0, 0, 0}),
                 request, distance);
  EXPECT_GT(distance.min_distance, -kOverlap + 1e-4);
}

TEST(MeshDepth, ConcaveRecessClassifiesTheNotchAsOutside) {
  constexpr double s = 0.05;
  constexpr double kDepth = 0.004;
  const auto l_shape = prepared(l_prism(s, 0.1));
  // A 20 mm cube in the notch, pressed 4 mm into the lower leg's top face;
  // its upper corners sit in the notch, 10 mm from the concave edge.
  const auto cube = prepared(box({0.01, 0.01, 0.01}, {1.2 * s + 0.01, s - kDepth + 0.01, 0.05}));
  ASSERT_TRUE(l_shape->closed);
  const auto depth = depth_of(*l_shape, at({0, 0, 0}), *cube, at({0, 0, 0}));
  ASSERT_EQ(depth.status, detail::MeshDepthStatus::kOk);
  ASSERT_TRUE(depth.found);
  EXPECT_NEAR(depth.depth_m, kDepth, 1e-9);
  EXPECT_NEAR(depth.normal_first_to_second.y(), 1.0, 1e-9);
}

TEST(MeshDepth, FullyInsideSpreadsFromOneVertex) {
  const auto big = prepared(box({0.1, 0.1, 0.1}, {0, 0, 0}));
  const auto small = prepared(box({0.01, 0.01, 0.01}, {0.05, 0, 0}));
  coal::CollisionRequest request;
  coal::CollisionResult collision;
  coal::collide(big->bvh.get(), at({0, 0, 0}), small->bvh.get(), at({0, 0, 0}), request,
                collision);
  EXPECT_FALSE(collision.isCollision());  // No surface crosses the other.
  const auto depth = detail::mesh_depth(*big, {}, detail::kNoSeedTriangle, *small, {},
                                        detail::kNoSeedTriangle);
  ASSERT_EQ(depth.status, detail::MeshDepthStatus::kOk);
  ASSERT_TRUE(depth.found);
  // The vertex at x = 0.04 is 0.06 from the face x = 0.1.
  EXPECT_NEAR(depth.depth_m, 0.06, 1e-9);
  EXPECT_NEAR(depth.normal_first_to_second.x(), 1.0, 1e-9);
}

TEST(MeshDepth, CornerIntoAFaceIsFoundByTheReversePass) {
  constexpr double kDepth = 0.003;
  // The pyramid's apex 3 mm into the top of a coarse plate whose own
  // vertices are all far outside the pyramid.
  const auto corner = prepared(pyramid(0.02, 0.03));
  const auto slab = prepared(plate(1));
  const auto depth =
      depth_of(*corner, at({0.01, 0.02, kPlateTop + 0.03 - kDepth}), *slab, at({0, 0, 0}));
  ASSERT_EQ(depth.status, detail::MeshDepthStatus::kOk);
  ASSERT_TRUE(depth.found);
  EXPECT_NEAR(depth.depth_m, kDepth, 1e-9);
  // Moving the plate down separates them.
  EXPECT_NEAR(depth.normal_first_to_second.z(), -1.0, 1e-9);
}

TEST(MeshDepth, OpenMeshIsInconclusive) {
  auto open = plate();
  open.triangles.pop_back();
  const auto open_shape = prepared(open);
  EXPECT_FALSE(open_shape->closed);
  const auto depth = depth_of(*open_shape, at({0, 0, 0}), *prepared(tube()), at({0, 0, 0}));
  EXPECT_EQ(depth.status, detail::MeshDepthStatus::kMeshNotClosed);
  EXPECT_FALSE(depth.found);
}

TEST(MeshDepth, InsideOutMeshIsFlippedAndReadsTheSame) {
  auto inside_out = plate();
  inside_out.flip();
  const auto flipped = prepared(inside_out);
  ASSERT_TRUE(flipped->closed);
  const auto tube_shape = prepared(tube());
  const auto expected = depth_of(*prepared(plate()), at({0, 0, 0}), *tube_shape, at({0, 0, 0}));
  const auto depth = depth_of(*flipped, at({0, 0, 0}), *tube_shape, at({0, 0, 0}));
  ASSERT_EQ(depth.status, detail::MeshDepthStatus::kOk);
  EXPECT_NEAR(depth.depth_m, expected.depth_m, 1e-12);
  EXPECT_NEAR((depth.normal_first_to_second - expected.normal_first_to_second).norm(), 0.0,
              1e-12);
}

TEST(MeshDepth, CapReachedIsInconclusive) {
  // About 600 vertices, all inside: more than the cap.
  const auto fine = prepared(box({0.02, 0.02, 0.02}, {0, 0, 0}, 10));
  const auto big = prepared(box({0.1, 0.1, 0.1}, {0, 0, 0}));
  const auto depth = detail::mesh_depth(*big, {}, detail::kNoSeedTriangle, *fine, {},
                                        detail::kNoSeedTriangle);
  EXPECT_EQ(depth.status, detail::MeshDepthStatus::kVertexCapReached);
  EXPECT_FALSE(depth.found);
}

TEST(MeshDepth, IdenticalInputGivesIdenticalDepth) {
  const auto plate_shape = prepared(plate());
  const auto tube_shape = prepared(tube());
  const auto pose = coal::Transform3s(
      Eigen::AngleAxisd(0.1, Eigen::Vector3d(1, 1, 0).normalized()).toRotationMatrix(),
      coal::Vec3s(0.003, -0.002, 0.0));
  const auto first = depth_of(*plate_shape, at({0, 0, 0}), *tube_shape, pose);
  // A different query in between leaves nothing behind.
  static_cast<void>(depth_of(*plate_shape, at({0, 0, 0}), *tube_shape, at({0, 0, 0})));
  const auto again = depth_of(*plate_shape, at({0, 0, 0}), *tube_shape, pose);
  const auto fresh = depth_of(*prepared(plate()), at({0, 0, 0}), *prepared(tube()), pose);
  for (const auto& other : {again, fresh}) {
    EXPECT_EQ(other.status, first.status);
    EXPECT_EQ(other.depth_m, first.depth_m);
    EXPECT_EQ(other.normal_first_to_second, first.normal_first_to_second);
  }
}

TEST(MeshDepth, OnlyTriangleMeshesArePrepared) {
  EXPECT_EQ(detail::prepare_mesh_depth(std::make_shared<coal::Box>(0.1, 0.1, 0.1)), nullptr);
  EXPECT_EQ(detail::prepare_mesh_depth(std::make_shared<coal::Sphere>(0.1)), nullptr);
}

// Through the engine: a robot link (a box) carries the tube, the plate is a
// world-fixed Scene object; both meshes are read from the content store.
class MeshDepthEngine : public ::testing::Test {
 protected:
  void SetUp() override {
    auto pattern = (std::filesystem::temp_directory_path() / "mesh-depth-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    root_ = pattern;
    std::ofstream(root_ / "robot.urdf") << R"(<robot name="robot"><link name="base"/>
      <link name="arm_link"><collision><geometry><box size="0.02 0.02 0.02"/></geometry></collision></link>
      <joint name="joint" type="prismatic"><parent link="base"/><child link="arm_link"/>
      <axis xyz="0 0 1"/><limit lower="-1" upper="1" effort="1" velocity="1"/></joint></robot>)";
    std::ofstream(root_ / "robot.srdf")
        << R"(<robot name="robot"><group name="arm"><joint name="joint"/></group></robot>)";
    std::filesystem::create_directories(root_ / "content");
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  std::string store(const Triangles& mesh) {
    const auto bytes = mesh.obj();
    const auto digest = manyforge::content_identity::sha256_digest(bytes);
    const auto path = root_ / "content" / manyforge::collision::scene_mesh_store_path(digest);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
    return digest;
  }

  manyforge::collision::ManyForgeCollisionEngine::SharedModelPtr model() {
    const int fd = ::open((root_ / "content").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    EXPECT_GE(fd, 0);
    auto shared = manyforge::collision::ManyForgeCollisionEngine::load_shared_model(
        "robot", root_ / "robot.urdf", root_ / "robot.srdf", {}, "arm", fd);
    ::close(fd);
    return shared;
  }

  manyforge::core::CollisionWorldSnapshot snapshot(
      const manyforge::collision::ManyForgeCollisionEngine::SharedModelPtr& shared,
      const std::string& tube_digest, const std::string& plate_digest, std::uint64_t revision) {
    namespace core = manyforge::core;
    const std::vector<manyforge::collision::RetainedComponentCollisionBodyProjectionInput> bodies{
        {core::ControllableComponentId("robot"), core::ModelInstanceId("robot"),
         core::ModelRevision("r1"), core::CollisionBodyRoleId("arm_link"), 0, "arm_link"}};
    const std::vector<manyforge::collision::RetainedComponentModelFrameProjectionInput> frames{
        {core::ControllableComponentId("robot"), core::ModelInstanceId("robot"),
         core::ModelRevision("r1"), core::FrameRoleId("base"), "base"}};
    const auto projection = manyforge::collision::ManyForgeCollisionEngine::
        project_retained_component_collision(shared, "base", bodies, frames);
    EXPECT_TRUE(projection.ok()) << projection.error().message;
    core::CollisionWorldSnapshot out;
    out.publisher_id = "mesh-depth-test";
    out.revision.revision_id = revision;
    out.revision.content_hash = "sha256:mesh-depth-" + std::to_string(revision);
    out.all_bodies_have_geometry = true;
    out.bodies = projection.value().component_bodies;
    out.component_model_frames = projection.value().component_model_frames;
    arm_body_ = out.bodies.front().body_id;
    const auto mesh_body = [&](const std::string& id, const std::string& digest,
                               std::optional<core::CollisionBodyId> parent) {
      core::CollisionWorldBody body;
      body.body_id = id;
      body.owner_kind = core::CollisionOwnerKind::kSceneResource;
      body.frame_id = parent ? "arm_link" : "world";
      body.parent_body_id = parent;
      body.pose = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
      core::CollisionGeometryDescriptor geometry;
      geometry.body_id = body.body_id;
      geometry.owner_kind = body.owner_kind;
      geometry.geometry_kind = core::CollisionGeometryKind::kTriangleMesh;
      geometry.local_pose = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
      geometry.source_uri = id + ".obj";
      geometry.source_hash = digest;
      geometry.mesh_scale = core::Vector3DTO{1.0, 1.0, 1.0};
      body.geometries.push_back(geometry);
      return body;
    };
    out.bodies.push_back(mesh_body("scene/plate/shape/0", plate_digest, std::nullopt));
    out.bodies.push_back(mesh_body("scene/tube/shape/0", tube_digest, arm_body_));
    std::sort(out.bodies.begin(), out.bodies.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.body_id < rhs.body_id; });
    out.body_count = out.bodies.size();
    out.geometry_count = 3;
    out.scene_resource_count = 2;
    out.contains_mesh_geometry = true;
    return out;
  }

  std::filesystem::path root_;
  manyforge::core::CollisionBodyId arm_body_;
};

TEST_F(MeshDepthEngine, SelectedMeshPairReadsTheDepthAndAPrimitivePairIsUnchanged) {
  namespace core = manyforge::core;
  using manyforge::collision::ManyForgeCollisionEngine;
  // The tube from z -0.05 to 0.05 on the link: at q its bottom is q - 0.05.
  const auto tube_digest = store(rod(0.01, -0.05, 0.05));
  const auto plate_digest = store(plate());
  const auto shared = model();
  ManyForgeCollisionEngine engine(shared, ManyForgeCollisionEngine::make_shared_scene_cache(shared));
  const auto world = snapshot(shared, tube_digest, plate_digest, 1);
  const std::vector<std::string> joints{"joint"};
  ASSERT_EQ(engine.prepare_runtime_query(joints), core::RuntimeCollisionQueryPrepareStatus::kReady);
  ASSERT_EQ(engine.prepare_runtime_snapshot(world),
            core::RuntimeCollisionQueryPrepareStatus::kReady);

  const core::CollisionPairId plate_tube{"scene/plate/shape/0", "scene/tube/shape/0"};
  const std::vector<core::CollisionPairId> mesh_pairs{plate_tube};
  const std::vector<double> pressed{kPlateTop - kOverlap + 0.05};
  const auto result = engine.check_runtime(
      world, {pressed, 0, 50'000'000, core::CollisionDistanceRequestMode::kSelectedCollidingPairs,
              mesh_pairs});
  ASSERT_EQ(result.status, core::RuntimeCollisionQueryStatus::kCollision);
  ASSERT_EQ(result.failure_reason, core::RuntimeCollisionQueryFailureReason::kNone);
  const auto observation = std::find_if(
      result.collision_pairs.begin(), result.collision_pairs.end(),
      [&](const auto& entry) { return *entry.pair == plate_tube; });
  ASSERT_NE(observation, result.collision_pairs.end());
  ASSERT_TRUE(observation->signed_clearance_m.has_value());
  EXPECT_NEAR(*observation->signed_clearance_m, -kOverlap, 1e-6);
  ASSERT_TRUE(observation->normal_a_to_b.has_value());
  EXPECT_NEAR((*observation->normal_a_to_b)[2], 1.0, 1e-9);

  // The link's box 3 mm into the plate: a primitive against a mesh keeps
  // coal's signed distance.
  const core::CollisionPairId arm_plate{arm_body_, "scene/plate/shape/0"};
  const std::vector<core::CollisionPairId> primitive_pairs{arm_plate};
  const std::vector<double> touching{kPlateTop + 0.01 - 0.003};
  const auto primitive = engine.check_runtime(
      world, {touching, 0, 50'000'000,
              core::CollisionDistanceRequestMode::kSelectedCollidingPairs, primitive_pairs});
  ASSERT_EQ(primitive.failure_reason, core::RuntimeCollisionQueryFailureReason::kNone);
  const auto box_entry = std::find_if(
      primitive.collision_pairs.begin(), primitive.collision_pairs.end(),
      [&](const auto& entry) { return *entry.pair == arm_plate; });
  ASSERT_NE(box_entry, primitive.collision_pairs.end());
  ASSERT_TRUE(box_entry->signed_clearance_m.has_value());
  coal::DistanceRequest request;
  request.enable_signed_distance = true;
  coal::DistanceResult expected;
  const auto plate_mesh = plate().mesh();
  const coal::Box link_box(0.02, 0.02, 0.02);
  coal::distance(&link_box, at({0, 0, touching[0]}), plate_mesh.get(), at({0, 0, 0}), request,
                 expected);
  EXPECT_NEAR(*box_entry->signed_clearance_m, expected.min_distance, 1e-6);
}

TEST_F(MeshDepthEngine, OpenMeshPairIsInconclusiveWithItsReason) {
  namespace core = manyforge::core;
  using manyforge::collision::ManyForgeCollisionEngine;
  auto open = plate();
  open.triangles.pop_back();
  const auto tube_digest = store(rod(0.01, -0.05, 0.05));
  const auto plate_digest = store(open);
  const auto shared = model();
  ManyForgeCollisionEngine engine(shared, ManyForgeCollisionEngine::make_shared_scene_cache(shared));
  const auto world = snapshot(shared, tube_digest, plate_digest, 1);
  const std::vector<std::string> joints{"joint"};
  ASSERT_EQ(engine.prepare_runtime_query(joints), core::RuntimeCollisionQueryPrepareStatus::kReady);
  ASSERT_EQ(engine.prepare_runtime_snapshot(world),
            core::RuntimeCollisionQueryPrepareStatus::kReady);
  const std::vector<core::CollisionPairId> pairs{{"scene/plate/shape/0", "scene/tube/shape/0"}};
  const std::vector<double> pressed{kPlateTop - kOverlap + 0.05};
  const auto result = engine.check_runtime(
      world, {pressed, 0, 50'000'000, core::CollisionDistanceRequestMode::kSelectedCollidingPairs,
              pairs});
  EXPECT_EQ(result.status, core::RuntimeCollisionQueryStatus::kInconclusive);
  EXPECT_EQ(result.failure_reason, core::RuntimeCollisionQueryFailureReason::kMeshNotClosed);
  // Plain collision still works with the open mesh.
  const auto plain = engine.check_runtime(
      world, {pressed, 0, 50'000'000, core::CollisionDistanceRequestMode::kNone, {}});
  EXPECT_EQ(plain.status, core::RuntimeCollisionQueryStatus::kCollision);
}

// Benchmark (OD-363 gate): the query time at the cap with a ~20k-triangle
// mesh. Run on the target machine:
//   test_mesh_depth --gtest_also_run_disabled_tests --gtest_filter='*Benchmark*'
TEST(MeshDepthBenchmark, DISABLED_QueryTimeAtTheCap) {
  // Two boxes of 12 * 41 * 41 = 20 172 triangles each, the second pressed
  // 1 mm into the first's top face: its whole bottom grid is inside, so the
  // first pass visits the full cap and stops there.
  constexpr int kGrid = 41;
  const auto fixed = prepared(box({0.1, 0.1, 0.1}, {0, 0, 0}, kGrid));
  const auto moving = prepared(box({0.05, 0.05, 0.05}, {0, 0, 0}, kGrid));
  const auto moving_pose = at({0.001, 0.002, 0.1 + 0.05 - 0.001});
  const auto tube_plate = prepared(plate(kGrid));
  const auto tube_shape = prepared(tube());

  // Only the depth query is timed; the seeds come from the collision pass.
  const auto measure = [](const char* name, const detail::MeshDepthShape& first,
                          const coal::Transform3s& first_pose,
                          const detail::MeshDepthShape& second,
                          const coal::Transform3s& second_pose) {
    constexpr int kRuns = 200;
    const auto seeds = seeds_of(first, first_pose, second, second_pose);
    std::vector<double> microseconds;
    microseconds.reserve(kRuns);
    volatile double sink = 0.0;
    for (int run = 0; run < kRuns; ++run) {
      const auto started = std::chrono::steady_clock::now();
      const auto result = detail::mesh_depth(first, mesh_pose(first_pose), seeds[0], second,
                                             mesh_pose(second_pose), seeds[1]);
      const auto elapsed = std::chrono::steady_clock::now() - started;
      sink = sink + result.depth_m;
      microseconds.push_back(std::chrono::duration<double, std::micro>(elapsed).count());
    }
    std::sort(microseconds.begin(), microseconds.end());
    std::cout << name << ": median " << microseconds[kRuns / 2] << " us, p99 "
              << microseconds[kRuns * 99 / 100] << " us, max " << microseconds.back()
              << " us\n";
    return microseconds.back();
  };
  const double at_cap = measure("one direction at the cap (256 vertices)", *fixed,
                                at({0, 0, 0}), *moving, moving_pose);
  EXPECT_EQ(depth_of(*fixed, at({0, 0, 0}), *moving, moving_pose).status,
            detail::MeshDepthStatus::kVertexCapReached);
  std::cout << "estimated worst case per pair (both directions at the cap): " << 2.0 * at_cap
            << " us\n";
  measure("tube 10.8 mm into a 20k-triangle plate", *tube_plate, at({0, 0, 0}), *tube_shape,
          at({0, 0, 0}));
}

}  // namespace
