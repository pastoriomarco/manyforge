#include <manyforge_roboplan/roboplan_projection_backend.hpp>

#include <manyforge_roboplan/collision_body_id_mapper.hpp>
#include <manyforge_roboplan/collision_diagnostics_capture.hpp>

#include <Eigen/Dense>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <type_traits>

#include <coal/collision_object.h>
#include <coal/BVH/BVH_model.h>
#include <coal/shape/convex.h>
#include <coal/shape/geometric_shapes.h>
#include <manyforge_core/collision/collision_policy_evaluator.hpp>
#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/types/content_hash.hpp>

#include <roboplan/core/scene_utils.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

// ----------------------------------------------------------------------------
// RoboPlan Scene mutation notes
//
// Older RoboPlan revisions required a ManyForge-side LIFO remove/replay
// workaround for scene-object mutation. The pinned RoboPlan main now owns the
// geometry bookkeeping invariants directly: removals repair the name->index
// map, reparenting updates parentJoint, and add*Geometry passes Pinocchio's
// JointIndex/FrameIndex arguments in the current order. This adapter therefore
// uses RoboPlan's direct scene APIs again and keeps object_states_ only as the
// semantic ManyForge view exposed to the Composer/native-viewer layers.
// ----------------------------------------------------------------------------

namespace manyforge::roboplan_backend {

namespace {

uint64_t steady_now_ns() {
  const auto tp = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             tp.time_since_epoch())
      .count();
}

Eigen::VectorXd vector_from_std(const std::vector<double>& positions) {
  Eigen::VectorXd q(positions.size());
  for (size_t i = 0; i < positions.size(); ++i) {
    q[static_cast<Eigen::Index>(i)] = positions[i];
  }
  return q;
}

std::vector<std::filesystem::path> split_env_paths(const char* value) {
  std::vector<std::filesystem::path> paths;
  if (value == nullptr || *value == '\0') {
    return paths;
  }
  std::stringstream stream(value);
  std::string item;
  while (std::getline(stream, item, ':')) {
    if (!item.empty()) {
      paths.emplace_back(item);
    }
  }
  return paths;
}

std::optional<std::string> object_id_from_normalized_body_id(
    const std::string& body_id) {
  constexpr std::string_view detached_prefix{"object/"};
  constexpr std::string_view attached_marker{"/attached/"};
  constexpr std::string_view shape_marker{"/shape/"};

  if (body_id.rfind(detached_prefix, 0) == 0) {
    const auto start = detached_prefix.size();
    const auto shape = body_id.find(shape_marker, start);
    if (shape != std::string::npos && shape > start) {
      return body_id.substr(start, shape - start);
    }
  }

  const auto attached = body_id.find(attached_marker);
  if (attached != std::string::npos) {
    const auto start = attached + attached_marker.size();
    const auto shape = body_id.find(shape_marker, start);
    if (shape != std::string::npos && shape > start) {
      return body_id.substr(start, shape - start);
    }
  }

  return std::nullopt;
}

std::optional<std::filesystem::path> first_existing_path(
    const std::vector<std::filesystem::path>& candidates) {
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (!candidate.empty() && std::filesystem::exists(candidate, ec)) {
      return std::filesystem::canonical(candidate, ec);
    }
  }
  return std::nullopt;
}

tl::expected<std::filesystem::path, std::string> resolve_mesh_resource_uri(
    const std::string& resource_uri) {
  if (resource_uri.empty()) {
    return tl::make_unexpected("mesh_resource_uri must not be empty");
  }

  std::string path_text = resource_uri;
  constexpr const char* kFilePrefix = "file://";
  constexpr const char* kPackagePrefix = "package://";
  if (path_text.rfind(kFilePrefix, 0) == 0) {
    path_text = path_text.substr(std::char_traits<char>::length(kFilePrefix));
  } else if (path_text.rfind(kPackagePrefix, 0) == 0) {
    const std::string rest =
        path_text.substr(std::char_traits<char>::length(kPackagePrefix));
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
      return tl::make_unexpected("Invalid package:// mesh URI: " + resource_uri);
    }
    const std::string package_name = rest.substr(0, slash);
    const std::filesystem::path package_rel_path = rest.substr(slash + 1);

    std::vector<std::filesystem::path> candidates;
    for (const auto& prefix : split_env_paths(std::getenv("AMENT_PREFIX_PATH"))) {
      candidates.push_back(prefix / "share" / package_name / package_rel_path);
    }
    for (const auto& prefix : split_env_paths(std::getenv("COLCON_PREFIX_PATH"))) {
      candidates.push_back(prefix / "share" / package_name / package_rel_path);
    }
    for (const auto& root : split_env_paths(std::getenv("ROS_PACKAGE_PATH"))) {
      candidates.push_back(root / package_name / package_rel_path);
      if (root.filename() == package_name) {
        candidates.push_back(root / package_rel_path);
      }
    }
    candidates.push_back(std::filesystem::path("/opt/ros/jazzy/share") /
                         package_name / package_rel_path);
    candidates.push_back(std::filesystem::path("/usr/local/share") /
                         package_name / package_rel_path);
    candidates.push_back(std::filesystem::path("/workspace") /
                         package_name / package_rel_path);

    if (auto resolved = first_existing_path(candidates)) {
      return *resolved;
    }
    return tl::make_unexpected("Could not resolve mesh URI: " + resource_uri);
  }

  const std::filesystem::path candidate(path_text);
  std::vector<std::filesystem::path> candidates;
  candidates.push_back(candidate);
  if (candidate.is_relative()) {
    candidates.push_back(std::filesystem::current_path() / candidate);
    candidates.push_back(std::filesystem::path("/workspace") / candidate);
  }

  if (auto resolved = first_existing_path(candidates)) {
    return *resolved;
  }
  return tl::make_unexpected("Mesh resource does not exist: " + resource_uri);
}

}  // namespace

RoboPlanProjectionBackend::RoboPlanProjectionBackend(
    std::shared_ptr<roboplan::Scene> scene, std::string group_name,
    std::shared_ptr<std::mutex> scene_mutex,
    std::optional<core::CollisionPolicySnapshot> initial_policy_snapshot,
    std::string robot_id)
    : scene_(std::move(scene)),
      base_scene_(scene_ ? std::make_shared<roboplan::Scene>(*scene_) : nullptr),
      group_name_(std::move(group_name)),
      scene_mutex_(std::move(scene_mutex)),
      mapper_(std::make_unique<RoboPlanCollisionBodyIdMapper>()) {
  if (!scene_ || !base_scene_) {
    throw std::invalid_argument("RoboPlan scene must not be null");
  }
  if (!scene_mutex_) {
    scene_mutex_ = std::make_shared<std::mutex>();
  }
  if (robot_id.empty()) {
    throw std::invalid_argument("RoboPlan projection robot id must not be empty");
  }
  mapper_robot_id_ = std::move(robot_id);
  policy_snapshot_ = std::move(initial_policy_snapshot);
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  publish_collision_world_revision_locked(
      core::CollisionWorldMutationSource::kProgramLoad,
      {},
      "roboplan_projection.constructor");
}

RoboPlanProjectionBackend::~RoboPlanProjectionBackend() = default;

namespace {

std::string stable_double(double value) {
  std::ostringstream oss;
  oss << std::setprecision(17) << value;
  return oss.str();
}

void append_pose_entries(std::vector<std::string>& entries,
                         const std::string& prefix,
                         const core::Pose3DTO& pose) {
  entries.push_back(prefix + ".px=" + stable_double(pose.position_m.x_m));
  entries.push_back(prefix + ".py=" + stable_double(pose.position_m.y_m));
  entries.push_back(prefix + ".pz=" + stable_double(pose.position_m.z_m));
  entries.push_back(prefix + ".qx=" + stable_double(pose.orientation.x));
  entries.push_back(prefix + ".qy=" + stable_double(pose.orientation.y));
  entries.push_back(prefix + ".qz=" + stable_double(pose.orientation.z));
  entries.push_back(prefix + ".qw=" + stable_double(pose.orientation.w));
}

void append_shape_entries(std::vector<std::string>& entries,
                          const std::string& prefix,
                          const core::CollisionShapeDTO& shape) {
  std::visit(
      [&](const auto& concrete) {
        using ShapeT = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<ShapeT, core::BoxShapeDTO>) {
          entries.push_back(prefix + ".shape=box");
          entries.push_back(prefix + ".x=" +
                            stable_double(concrete.dimensions_m.x_m));
          entries.push_back(prefix + ".y=" +
                            stable_double(concrete.dimensions_m.y_m));
          entries.push_back(prefix + ".z=" +
                            stable_double(concrete.dimensions_m.z_m));
        } else if constexpr (std::is_same_v<ShapeT, core::SphereShapeDTO>) {
          entries.push_back(prefix + ".shape=sphere");
          entries.push_back(prefix + ".r=" + stable_double(concrete.radius_m));
        } else if constexpr (std::is_same_v<ShapeT, core::CylinderShapeDTO>) {
          entries.push_back(prefix + ".shape=cylinder");
          entries.push_back(prefix + ".r=" + stable_double(concrete.radius_m));
          entries.push_back(prefix + ".h=" + stable_double(concrete.height_m));
        } else if constexpr (std::is_same_v<ShapeT, core::MeshShapeDTO>) {
          entries.push_back(prefix + ".shape=mesh");
          entries.push_back(prefix + ".uri=" + concrete.resource_uri);
          entries.push_back(prefix + ".sx=" + stable_double(concrete.scale.x_m));
          entries.push_back(prefix + ".sy=" + stable_double(concrete.scale.y_m));
          entries.push_back(prefix + ".sz=" + stable_double(concrete.scale.z_m));
        }
      },
      shape);
}

std::string collision_world_body_cache_fingerprint(
    const core::CollisionWorldBody& body) {
  std::vector<std::string> entries;
  entries.push_back("body=" + body.body_id);
  entries.push_back("owner=" +
                    std::to_string(static_cast<int>(body.owner_kind)));
  entries.push_back("parent=" + body.parent_link.value_or(""));
  entries.push_back("frame=" + body.frame_id);
  append_pose_entries(entries, "pose", body.pose);
  entries.push_back(
      "freshness_set=" +
      std::string(body.source_freshness_budget_ns.has_value() ? "1" : "0"));
  entries.push_back("source_id=" + body.source_id.value_or(""));
  if (body.source_freshness_budget_ns.has_value()) {
    entries.push_back(
        "freshness=" + std::to_string(*body.source_freshness_budget_ns));
  }
  for (size_t i = 0; i < body.geometries.size(); ++i) {
    const auto& geom = body.geometries[i];
    const std::string prefix = "geom." + std::to_string(i);
    entries.push_back(prefix + ".body=" + geom.body_id);
    entries.push_back(prefix + ".owner=" +
                      std::to_string(static_cast<int>(geom.owner_kind)));
    entries.push_back(prefix + ".kind=" +
                      std::to_string(static_cast<int>(geom.geometry_kind)));
    append_pose_entries(entries, prefix + ".local_pose", geom.local_pose);
    if (geom.box_dimensions_m.has_value()) {
      entries.push_back(prefix + ".box.x=" +
                        stable_double(geom.box_dimensions_m->x_m));
      entries.push_back(prefix + ".box.y=" +
                        stable_double(geom.box_dimensions_m->y_m));
      entries.push_back(prefix + ".box.z=" +
                        stable_double(geom.box_dimensions_m->z_m));
    }
    entries.push_back(prefix + ".sphere_r=" +
                      stable_double(geom.sphere_radius_m.value_or(-1.0)));
    entries.push_back(prefix + ".cylinder_r=" +
                      stable_double(geom.cylinder_radius_m.value_or(-1.0)));
    entries.push_back(prefix + ".cylinder_h=" +
                      stable_double(geom.cylinder_height_m.value_or(-1.0)));
    entries.push_back(prefix + ".capsule_r=" +
                      stable_double(geom.capsule_radius_m.value_or(-1.0)));
    entries.push_back(prefix + ".capsule_h=" +
                      stable_double(geom.capsule_height_m.value_or(-1.0)));
    if (geom.mesh_scale.has_value()) {
      entries.push_back(prefix + ".mesh_sx=" +
                        stable_double(geom.mesh_scale->x_m));
      entries.push_back(prefix + ".mesh_sy=" +
                        stable_double(geom.mesh_scale->y_m));
      entries.push_back(prefix + ".mesh_sz=" +
                        stable_double(geom.mesh_scale->z_m));
    }
    entries.push_back(prefix + ".source_uri=" + geom.source_uri.value_or(""));
    entries.push_back(prefix + ".source_hash=" + geom.source_hash.value_or(""));
    std::vector<std::string> labels = geom.quality_labels;
    std::sort(labels.begin(), labels.end());
    for (const auto& label : labels) {
      entries.push_back(prefix + ".quality=" + label);
    }
    entries.push_back(prefix + ".triangles=" +
                      std::to_string(geom.triangle_count.value_or(-1)));
    entries.push_back(prefix + ".convex_parts=" +
                      std::to_string(geom.convex_part_count.value_or(-1)));
    entries.push_back(prefix + ".memory=" +
                      std::to_string(geom.memory_estimate_bytes.value_or(-1)));
  }
  std::ostringstream oss;
  for (const auto& entry : entries) {
    oss << entry << '\n';
  }
  return oss.str();
}

std::string robot_link_parent_body_id(const std::string& robot_id,
                                      const std::string& link_name) {
  return "robot/" + robot_id + "/link/" + link_name;
}

bool robot_collision_body_id_matches_link(const core::CollisionBodyId& body_id,
                                          const std::string& robot_id,
                                          const std::string& link_name) {
  const std::string prefix =
      robot_link_parent_body_id(robot_id, link_name) + "/collision/";
  return body_id.rfind(prefix, 0) == 0;
}

std::optional<std::string> robot_link_from_collision_body_id(
    const core::CollisionBodyId& body_id,
    const std::string& robot_id) {
  const std::string prefix = "robot/" + robot_id + "/link/";
  if (body_id.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  const auto begin = prefix.size();
  const auto collision_pos = body_id.find("/collision/", begin);
  if (collision_pos == std::string::npos || collision_pos == begin) {
    return std::nullopt;
  }
  return body_id.substr(begin, collision_pos - begin);
}

std::optional<core::ObservedSceneObjectRole> observed_scene_role_from_string(
    const std::optional<std::string>& raw) {
  if (!raw.has_value()) {
    return std::nullopt;
  }
  if (*raw == "static_obstacle") {
    return core::ObservedSceneObjectRole::kStaticObstacle;
  }
  if (*raw == "snapshot_obstacle") {
    return core::ObservedSceneObjectRole::kSnapshotObstacle;
  }
  if (*raw == "observed_target") {
    return core::ObservedSceneObjectRole::kObservedTarget;
  }
  if (*raw == "observed_dynamic_obstacle") {
    return core::ObservedSceneObjectRole::kObservedDynamicObstacle;
  }
  if (*raw == "attached_object") {
    return core::ObservedSceneObjectRole::kAttachedObject;
  }
  if (*raw == "support_contact") {
    return core::ObservedSceneObjectRole::kSupportContact;
  }
  if (*raw == "visualization_only") {
    return core::ObservedSceneObjectRole::kVisualizationOnly;
  }
  if (*raw == "robot_runtime_state") {
    return core::ObservedSceneObjectRole::kRobotRuntimeState;
  }
  return std::nullopt;
}

bool non_negative_optional_double(const std::optional<double>& value) {
  return !value.has_value() ||
         (std::isfinite(*value) && *value >= 0.0);
}

bool optional_text_blank(const std::optional<std::string>& value) {
  return value.has_value() &&
         std::all_of(value->begin(), value->end(), [](unsigned char ch) {
           return std::isspace(ch) != 0;
         });
}

bool vector3_equal(const core::Vector3DTO& lhs, const core::Vector3DTO& rhs) {
  return lhs.x_m == rhs.x_m && lhs.y_m == rhs.y_m && lhs.z_m == rhs.z_m;
}

bool quaternion_equal(const core::QuaternionDTO& lhs,
                      const core::QuaternionDTO& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z &&
         lhs.w == rhs.w;
}

bool pose_equal(const core::Pose3DTO& lhs, const core::Pose3DTO& rhs) {
  return vector3_equal(lhs.position_m, rhs.position_m) &&
         quaternion_equal(lhs.orientation, rhs.orientation);
}

bool link_is_static_wrt_group(const pinocchio::Model& model,
                              const std::string& link_name,
                              const std::set<std::string>& group_joint_names) {
  if (group_joint_names.empty() || !model.existFrame(link_name)) {
    return false;
  }
  auto joint_id = model.frames[model.getFrameId(link_name)].parentJoint;
  while (joint_id > 0 && static_cast<std::size_t>(joint_id) < model.parents.size()) {
    if (static_cast<std::size_t>(joint_id) < model.names.size() &&
        static_cast<std::size_t>(joint_id) < model.nqs.size() &&
        model.nqs[joint_id] > 0 &&
        group_joint_names.find(model.names[joint_id]) != group_joint_names.end()) {
      return false;
    }
    joint_id = model.parents[joint_id];
  }
  return true;
}

core::Vector3DTO vector3_from_eigen(const Eigen::Vector3d& value) {
  core::Vector3DTO out;
  out.x_m = value.x();
  out.y_m = value.y();
  out.z_m = value.z();
  return out;
}

std::string effective_mesh_source_hash(const core::MeshShapeDTO& mesh) {
  core::ContentHasher h;
  h.mix("mesh_shape")
      .mix(mesh.resource_uri)
      .mix(mesh.scale.x_m)
      .mix(mesh.scale.y_m)
      .mix(mesh.scale.z_m);
  return h.finalize();
}

void add_common_quality_label(core::CollisionGeometryDescriptor& descriptor,
                              std::string label) {
  descriptor.quality_labels.push_back(std::move(label));
  std::sort(descriptor.quality_labels.begin(),
            descriptor.quality_labels.end());
  descriptor.quality_labels.erase(
      std::unique(descriptor.quality_labels.begin(),
                  descriptor.quality_labels.end()),
      descriptor.quality_labels.end());
}

core::CollisionGeometryDescriptor descriptor_from_coal_geometry(
    const core::CollisionBodyId& body_id,
    const pinocchio::GeometryObject& geometry_object,
    const core::Pose3DTO& local_pose) {
  core::CollisionGeometryDescriptor descriptor;
  descriptor.body_id = body_id;
  descriptor.owner_kind = core::CollisionOwnerKind::kRobotLink;
  descriptor.local_pose = local_pose;
  descriptor.source_hash = geometry_object.name;
  descriptor.quality_labels = {"robot_collision"};

  const auto& geometry = geometry_object.geometry;
  if (!geometry) {
    descriptor.geometry_kind = core::CollisionGeometryKind::kCompound;
    add_common_quality_label(descriptor, "missing_backend_geometry");
    return descriptor;
  }

  auto apply_box_proxy = [&](const Eigen::Vector3d& min,
                             const Eigen::Vector3d& max,
                             const std::string& source_label) -> bool {
    const Eigen::Vector3d size = max - min;
    const Eigen::Vector3d center = 0.5 * (max + min);
    if (!(size.array().isFinite()).all() ||
        !(center.array().isFinite()).all() ||
        !(size.array() > 0.0).all()) {
      return false;
    }

    Eigen::Quaterniond q(local_pose.orientation.w, local_pose.orientation.x,
                         local_pose.orientation.y, local_pose.orientation.z);
    if (!std::isfinite(q.x()) || !std::isfinite(q.y()) ||
        !std::isfinite(q.z()) || !std::isfinite(q.w()) ||
        q.norm() <= 0.0) {
      return false;
    }
    q.normalize();

    descriptor.local_pose = local_pose;
    const Eigen::Vector3d translated =
        q.toRotationMatrix() * center +
        Eigen::Vector3d(local_pose.position_m.x_m,
                        local_pose.position_m.y_m,
                        local_pose.position_m.z_m);
    descriptor.local_pose.position_m = vector3_from_eigen(translated);
    descriptor.geometry_kind = core::CollisionGeometryKind::kBox;
    descriptor.box_dimensions_m = vector3_from_eigen(size);
    add_common_quality_label(descriptor, "primitive");
    add_common_quality_label(descriptor, "conservative_proxy");
    add_common_quality_label(descriptor, "warning_conservative_aabb_proxy");
    add_common_quality_label(descriptor, source_label);
    return true;
  };

  auto apply_points_proxy = [&](const auto& points,
                                std::size_t count,
                                const std::string& source_label) -> bool {
    if (count == 0 || points == nullptr || points->size() < count) {
      return false;
    }
    Eigen::Vector3d min = points->at(0);
    Eigen::Vector3d max = points->at(0);
    for (std::size_t i = 1; i < count; ++i) {
      min = min.cwiseMin(points->at(i));
      max = max.cwiseMax(points->at(i));
    }
    return apply_box_proxy(min, max, source_label);
  };

  auto apply_backend_aabb_proxy = [&]() -> bool {
    if (geometry->aabb_radius < 0.0) {
      return false;
    }
    return apply_box_proxy(geometry->aabb_local.min_,
                           geometry->aabb_local.max_,
                           "backend_aabb_proxy");
  };

  auto apply_mesh_points_proxy = [&]() -> bool {
    const auto* bvh =
        dynamic_cast<const coal::BVHModelBase*>(geometry.get());
    if (bvh == nullptr) {
      return false;
    }
    return apply_points_proxy(bvh->vertices,
                              static_cast<std::size_t>(bvh->num_vertices),
                              "backend_vertex_aabb_proxy");
  };

  auto apply_convex_points_proxy = [&]() -> bool {
    const auto* convex =
        dynamic_cast<const coal::ConvexBase*>(geometry.get());
    if (convex == nullptr) {
      return false;
    }
    return apply_points_proxy(convex->points,
                              static_cast<std::size_t>(convex->num_points),
                              "backend_vertex_aabb_proxy");
  };

  auto apply_mesh_resource = [&]() -> bool {
    if (geometry_object.meshPath.empty()) {
      return false;
    }
    if (!(geometry_object.meshScale.array().isFinite()).all() ||
        !(geometry_object.meshScale.array() > 0.0).all()) {
      return false;
    }
    descriptor.geometry_kind = core::CollisionGeometryKind::kTriangleMesh;
    descriptor.source_uri = geometry_object.meshPath;
    descriptor.mesh_scale = vector3_from_eigen(geometry_object.meshScale);
    add_common_quality_label(descriptor, "mesh");
    add_common_quality_label(descriptor, "backend_mesh");
    return true;
  };

  switch (geometry->getNodeType()) {
    case coal::GEOM_BOX: {
      const auto* box = dynamic_cast<const coal::Box*>(geometry.get());
      if (box != nullptr) {
        descriptor.geometry_kind = core::CollisionGeometryKind::kBox;
        descriptor.box_dimensions_m = vector3_from_eigen(2.0 * box->halfSide);
        add_common_quality_label(descriptor, "primitive");
        return descriptor;
      }
      break;
    }
    case coal::GEOM_SPHERE: {
      const auto* sphere = dynamic_cast<const coal::Sphere*>(geometry.get());
      if (sphere != nullptr) {
        descriptor.geometry_kind = core::CollisionGeometryKind::kSphere;
        descriptor.sphere_radius_m = sphere->radius;
        add_common_quality_label(descriptor, "primitive");
        return descriptor;
      }
      break;
    }
    case coal::GEOM_CYLINDER: {
      const auto* cylinder =
          dynamic_cast<const coal::Cylinder*>(geometry.get());
      if (cylinder != nullptr) {
        descriptor.geometry_kind = core::CollisionGeometryKind::kCylinder;
        descriptor.cylinder_radius_m = cylinder->radius;
        descriptor.cylinder_height_m = 2.0 * cylinder->halfLength;
        add_common_quality_label(descriptor, "primitive");
        return descriptor;
      }
      break;
    }
    case coal::GEOM_CAPSULE: {
      const auto* capsule = dynamic_cast<const coal::Capsule*>(geometry.get());
      if (capsule != nullptr) {
        const double radius = capsule->radius;
        const double half_length = capsule->halfLength;
        if (std::isfinite(radius) && std::isfinite(half_length) &&
            radius > 0.0 && half_length > 0.0) {
          // The Phase-2 manyforge_collision engine is intentionally primitive-box/sphere/
          // cylinder only. Represent backend robot capsules as explicit
          // conservative boxes so the snapshot stays consumable without
          // silently dropping robot collision volume.
          const Eigen::Vector3d min(-radius, -radius,
                                    -(half_length + radius));
          const Eigen::Vector3d max(radius, radius,
                                    half_length + radius);
          if (apply_box_proxy(min, max, "backend_capsule_aabb_proxy")) {
            return descriptor;
          }
        }
      }
      break;
    }
    case coal::GEOM_CONVEX:
      if (apply_mesh_resource() || apply_convex_points_proxy() ||
          apply_backend_aabb_proxy()) {
        return descriptor;
      }
      descriptor.geometry_kind = core::CollisionGeometryKind::kConvexMesh;
      add_common_quality_label(descriptor, "convex");
      return descriptor;
    default:
      break;
  }

  if (apply_mesh_resource()) {
    return descriptor;
  }

  // Industrial manyforge_collision workers must not rely on backend mesh BVH
  // behavior that emits unstructured Coal numerical warnings. For robot-link
  // mesh or compound collision geometry, publish a deterministic conservative
  // AABB proxy in the geometry frame. This keeps the snapshot consumable by the
  // primitive ManyForge-owned engine and makes the approximation explicit through
  // quality labels.
  if (apply_mesh_points_proxy() || apply_convex_points_proxy() ||
      apply_backend_aabb_proxy()) {
    return descriptor;
  }

  descriptor.geometry_kind = core::CollisionGeometryKind::kCompound;
  add_common_quality_label(descriptor, "unsupported_backend_geometry");
  return descriptor;
}

/// Deterministic fingerprint over the body inventory that affects body-id
/// assignment. Two scenes with identical (object_id, attached) sets in any
/// order produce identical fingerprints. Includes raw robot collision-body
/// mappings, so a robot model/deployment change triggers a rebuild. The
/// fingerprint changes whenever a rebuild is needed (object
/// added/removed/attached/detached, robot mapping changed); it does NOT change
/// for pose-only updates that leave the body inventory intact.
std::string compute_body_identity_fingerprint(
    const std::string& robot_id,
    const std::vector<RoboPlanProjectionBackend::SceneObjectSnapshot>& objects,
    const std::vector<std::pair<std::string, core::CollisionBodyId>>&
        robot_body_mappings) {
  std::vector<std::string> entries;
  entries.reserve(objects.size() + robot_body_mappings.size());
  for (const auto& obj : objects) {
    entries.push_back("object:" + obj.object_id + ":" +
                      (obj.attached ? "A" : "D"));
  }
  for (const auto& [raw_name, body_id] : robot_body_mappings) {
    entries.push_back("robot:" + raw_name + ":" + body_id);
  }
  std::sort(entries.begin(), entries.end());

  core::ContentHasher h;
  h.mix("manyforge_roboplan/body_identity_fingerprint");
  h.mix(robot_id);
  for (const auto& e : entries) {
    h.mix(e);
  }
  return h.finalize();
}

core::CollisionDecisionResult merge_policy_collision_decision(
    core::CollisionDecisionResult engine_decision,
    core::CollisionDecisionResult policy_decision) {
  // Complete policy classification may allow raw engine collisions
  // (static-static, touch links, support contacts). Incomplete diagnostics are
  // not a policy classification, so they remain fail-closed and cannot be
  // downgraded by an empty/dispatch policy result.
  if (engine_decision == core::CollisionDecisionResult::kInconclusiveFail ||
      engine_decision == core::CollisionDecisionResult::kStopRequested) {
    return engine_decision;
  }
  return policy_decision;
}

std::string collision_engine_status_text(core::CollisionEngineStatus status) {
  switch (status) {
    case core::CollisionEngineStatus::kOk:
      return "ok";
    case core::CollisionEngineStatus::kTimeout:
      return "timeout";
    case core::CollisionEngineStatus::kUnsupportedGeometry:
      return "unsupported_geometry";
    case core::CollisionEngineStatus::kDiagnosticIncomplete:
      return "diagnostic_incomplete";
    case core::CollisionEngineStatus::kCancelled:
      return "cancelled";
    case core::CollisionEngineStatus::kEngineError:
      return "engine_error";
  }
  return "unknown";
}

std::string collision_decision_text(core::CollisionDecisionResult decision) {
  switch (decision) {
    case core::CollisionDecisionResult::kPass:
      return "pass";
    case core::CollisionDecisionResult::kFail:
      return "fail";
    case core::CollisionDecisionResult::kWarn:
      return "warn";
    case core::CollisionDecisionResult::kStopRequested:
      return "stop_requested";
    case core::CollisionDecisionResult::kInconclusiveFail:
      return "inconclusive_fail";
  }
  return "unknown";
}

bool collision_pairs_equal(const std::vector<core::CollisionPairId>& lhs,
                           const std::vector<core::CollisionPairId>& rhs) {
  return lhs == rhs;
}

bool roboplan_normalized_supports_required_capability(
    const std::string& capability) {
  return capability == "binary_collision" || capability == "body_pair_only";
}

}  // namespace

core::SceneEditResultDTO RoboPlanProjectionBackend::apply_collision_policy(
    const core::SetCollisionPolicyRequestDTO& request) {
  if (request.policy.policy_id.empty()) {
    auto result = core::SceneEditResultDTO{
        false,
        core::ErrorDTO{core::kErrorInvalidRequest,
                       "Collision policy id must not be empty",
                       {{"reason", "collision_policy_id_empty"}}}};
    result.rollback_proven = true;
    return result;
  }
  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    {
      std::scoped_lock lock(evaluator_mutex_);
      policy_snapshot_ = request.policy;
    }
    publish_collision_world_revision_locked(
        core::CollisionWorldMutationSource::kPolicyUpdate, {},
        "scene_authority.set_collision_policy");
    core::SceneEditResultDTO result;
    result.success = true;
    result.update_status = "geometry_changed";
    result.collision_world_revision_changed = true;
    return result;
  } catch (const std::exception& error) {
    return core::SceneEditResultDTO{
        false,
        core::ErrorDTO{core::kErrorInternalError,
                       "RoboPlan collision policy projection failed",
                       {{"reason", "collision_policy_projection_failed"},
                        {"exception", error.what()}}}};
  }
}

core::CollisionPolicySnapshot
RoboPlanProjectionBackend::configured_collision_policy() const {
  std::scoped_lock lock(evaluator_mutex_);
  return policy_snapshot_.value_or(core::CollisionPolicySnapshot{});
}

void RoboPlanProjectionBackend::configure_diagnostic_compare_engine(
    std::shared_ptr<core::ICollisionQueryEngine> engine) {
  std::scoped_lock lock(compare_engine_mutex_);
  compare_engine_ = std::move(engine);
}

void RoboPlanProjectionBackend::configure_diagnostic_mismatch_emitter(
    MismatchRecordEmitter emitter) {
  std::scoped_lock lock(compare_engine_mutex_);
  mismatch_record_emitter_ = std::move(emitter);
}

core::CollisionWorldRevision
RoboPlanProjectionBackend::published_collision_world_revision() const {
  std::lock_guard<std::mutex> lock(published_revision_mutex_);
  return published_revision_;
}

core::CollisionWorldSnapshotPtr RoboPlanProjectionBackend::snapshot() const {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  if (snapshot_cache_ &&
      snapshot_cache_revision_id_ == world_revision_id_ &&
      snapshot_cache_content_hash_ == world_content_hash_) {
    return snapshot_cache_;
  }

  auto next = build_collision_world_snapshot_locked();
  if (!next) {
    snapshot_cache_.reset();
    snapshot_cache_revision_id_ = 0;
    snapshot_cache_content_hash_.clear();
    return nullptr;
  }

  snapshot_cache_revision_id_ = world_revision_id_;
  snapshot_cache_content_hash_ = world_content_hash_;
  snapshot_cache_ = std::move(next);
  return snapshot_cache_;
}

std::vector<core::CollisionWorldLedgerEntry>
RoboPlanProjectionBackend::recent_collision_world_ledger(std::size_t limit) const {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  std::vector<core::CollisionWorldLedgerEntry> out;
  if (world_ledger_.empty()) {
    return out;
  }
  const std::size_t take =
      (limit == 0 || limit >= world_ledger_.size()) ? world_ledger_.size() : limit;
  out.reserve(take);
  // Newest-first: walk from the back of the deque.
  auto it = world_ledger_.crbegin();
  for (std::size_t i = 0; i < take && it != world_ledger_.crend(); ++i, ++it) {
    out.push_back(*it);
  }
  return out;
}

void RoboPlanProjectionBackend::publish_collision_world_revision_locked(
    core::CollisionWorldMutationSource mutation_source,
    const std::vector<std::string>& affected_body_ids,
    std::string authority_source,
    std::optional<std::string> session_id,
    bool advance_revision,
    bool advance_epochs) {
  // Caller MUST hold scene_mutex_. Build the new revision id + content hash
  // and append a ledger entry. content_hash is a pure digest of the collision
  // world content; revision_id and the ledger fields provide sequence
  // identity for repeated mutations that leave content unchanged.
  const std::uint64_t previous_revision_id = world_revision_id_;
  if (advance_revision) {
    world_revision_id_ += 1;
  }
  const bool pose_only_mutation =
      mutation_source == core::CollisionWorldMutationSource::kPoseSourceApply ||
      mutation_source == core::CollisionWorldMutationSource::kProjectionRebuild;
  const bool policy_only_mutation =
      mutation_source == core::CollisionWorldMutationSource::kPolicyUpdate;
  const bool policy_semantics_mutation =
      policy_only_mutation ||
      mutation_source == core::CollisionWorldMutationSource::kProgramLoad ||
      mutation_source == core::CollisionWorldMutationSource::kDeploymentLoad ||
      mutation_source == core::CollisionWorldMutationSource::kAttach ||
      mutation_source == core::CollisionWorldMutationSource::kDetach;
  // Plain geometry add/remove changes the derived live policy snapshot but not
  // the semantic policy epoch used by frozen snapshot-lease admission.
  if (advance_epochs && !pose_only_mutation && !policy_only_mutation) {
    ++topology_epoch_;
  }
  if (advance_epochs && policy_semantics_mutation) {
    ++policy_epoch_;
  }

  std::string robot_id;
  {
    std::scoped_lock map_lock(mapper_mutex_);
    robot_id = mapper_robot_id_;
  }
  auto next_snapshot = build_collision_world_snapshot_locked();
  if (next_snapshot) {
    world_content_hash_ = next_snapshot->revision.content_hash;
    snapshot_cache_revision_id_ = world_revision_id_;
    snapshot_cache_content_hash_ = world_content_hash_;
    snapshot_cache_ = std::move(next_snapshot);
  } else {
    world_content_hash_ = compute_collision_world_content_hash_locked(robot_id);
    snapshot_cache_.reset();
    snapshot_cache_revision_id_ = 0;
    snapshot_cache_content_hash_.clear();
  }

  core::CollisionWorldLedgerEntry entry;
  entry.revision.revision_id = world_revision_id_;
  entry.revision.content_hash = world_content_hash_;
  {
    std::lock_guard<std::mutex> revision_lock(published_revision_mutex_);
    published_revision_ = entry.revision;
  }
  core::CollisionWorldPolicySnapshotBundle runtime_bundle;
  runtime_bundle.world = snapshot_cache_;
  if (runtime_bundle.world) {
    runtime_bundle.policy = build_collision_policy_snapshot_locked(robot_id);
  }
  runtime_bundle.body_freshness = runtime_body_freshness_locked();
  (void)publish_runtime_query_bundle_locked(
      core::build_runtime_collision_query_bundle(
          std::move(runtime_bundle), ++published_runtime_bundle_epoch_));
  entry.previous_revision_id = previous_revision_id;
  entry.mutation_source = mutation_source;
  entry.affected_body_ids = affected_body_ids;
  entry.timestamp_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  entry.authority_source = std::move(authority_source);
  entry.session_id = std::move(session_id);

  world_ledger_.push_back(std::move(entry));
  while (world_ledger_.size() > kCollisionWorldLedgerCapacity) {
    world_ledger_.pop_front();
  }
}

core::CollisionResult RoboPlanProjectionBackend::compare_collision_diagnostic(
    const core::CollisionQuery& query) {
  const auto t_start = std::chrono::steady_clock::now();
  std::string robot_id;
  {
    std::scoped_lock lock(mapper_mutex_);
    robot_id = mapper_robot_id_;
  }

  for (const auto& capability : query.required_capabilities) {
    if (!roboplan_normalized_supports_required_capability(capability)) {
      core::CollisionResult result;
      result.query_id = query.query_id;
      result.policy_revision = query.policy_revision;
      result.surface = query.surface;
      result.state_time_ns = query.state_time_ns;
      result.sampling_mode = query.sampling_mode;
      result.engine_id = "roboplan_scene";
      result.engine_version = "unknown";
      if (query.collision_world_revision.revision_id != 0 ||
          !query.collision_world_revision.content_hash.empty()) {
        result.collision_world_revision = query.collision_world_revision;
      } else {
        result.collision_world_revision =
            published_collision_world_revision();
      }
      result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.query_duration_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - t_start)
              .count();
      return result;
    }
  }

  std::vector<SceneObjectSnapshot> scene_objects;
  std::vector<std::pair<std::string, core::CollisionBodyId>> robot_mappings;
  core::CollisionCheckResultDTO legacy_result;
  CollisionDiagnostics diagnostics;
  core::CollisionWorldRevision checked_revision;

  {
    // Snapshot the body inventory and run the planner-native diagnostic query while
    // holding the same scene lock. This keeps the normalized diagnostic mapping
    // consistent with the scene that was actually checked. We do not hold the
    // mapper mutex while touching RoboPlan/Pinocchio state.
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    scene_objects = scene_objects_locked();
    robot_mappings = robot_body_mappings_locked(robot_id);
    checked_revision =
        core::CollisionWorldRevision{world_revision_id_, world_content_hash_};

    Eigen::VectorXd q = vector_from_std(query.robot_state.positions_rad);
    if (q.size() != scene_->getCurrentJointPositions().size()) {
      q = scene_->toFullJointPositions(group_name_, q);
    }
    legacy_result.in_collision = scene_->hasCollisions(q);
    if (legacy_result.in_collision) {
      diagnostics = collect_collision_diagnostics_locked(q);
      if (!diagnostics.pairs.empty()) {
        legacy_result.contact_body_a = diagnostics.pairs.front().canonical_pair.first;
        legacy_result.contact_body_b = diagnostics.pairs.front().canonical_pair.second;
      }
    }
  }
  const auto t_end = std::chrono::steady_clock::now();

  RoboPlanCollisionBodyIdMapper result_mapper;
  result_mapper.rebuild(robot_id, scene_objects, robot_mappings);
  {
    std::scoped_lock lock(mapper_mutex_);
    std::string fingerprint = compute_body_identity_fingerprint(
        robot_id, scene_objects, robot_mappings);
    if (fingerprint != mapper_scene_fingerprint_) {
      mapper_->rebuild(robot_id, scene_objects, robot_mappings);
      mapper_scene_fingerprint_ = std::move(fingerprint);
    }
  }

  // Build the normalized CollisionResult next to the legacy result.
  core::CollisionResult result;
  result.in_collision = legacy_result.in_collision;
  result.contact_body_a = legacy_result.contact_body_a;
  result.contact_body_b = legacy_result.contact_body_b;
  result.query_id = query.query_id;
  result.policy_revision = query.policy_revision;
  result.surface = query.surface;
  result.state_time_ns = query.state_time_ns;
  result.sampling_mode = query.sampling_mode;
  result.engine_id = "roboplan_scene";
  result.engine_version = "unknown";
  result.query_duration_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start)
          .count();
  if (query.collision_world_revision.revision_id != 0 ||
      !query.collision_world_revision.content_hash.empty()) {
    // Caller supplied an authoritative revision (e.g. Chunk 5's validation
    // coordinator). Echo it back so failure archives can correlate.
    result.collision_world_revision = query.collision_world_revision;
  } else {
    // Chunk 2: capture the in-context publisher revision inside the same
    // scene critical section that executed hasCollisions(). Do not re-read
    // it here; a concurrent mutation after the check would otherwise tag
    // this result with a revision it did not actually validate.
    if (checked_revision.revision_id != 0 ||
        !checked_revision.content_hash.empty()) {
      result.collision_world_revision = checked_revision;
    }
  }
  // Chunk 1: distance queries are not supported; min_clearance fields remain
  // null (per spec/210 §3.8.1).

  bool any_unknown = legacy_result.in_collision && diagnostics.pairs.empty();
  {
    for (const auto& raw_pair : diagnostics.pairs) {
      auto a = result_mapper.lookup(raw_pair.canonical_pair.first);
      auto b = result_mapper.lookup(raw_pair.canonical_pair.second);
      if (a && b) {
        core::CollisionPairId pair{*a, *b};
        if (pair.body_b < pair.body_a) {
          std::swap(pair.body_a, pair.body_b);
        }
        result.normalized_pairs.push_back(pair);
      } else {
        any_unknown = true;
      }
    }
  }

  std::sort(result.normalized_pairs.begin(), result.normalized_pairs.end(),
            [](const auto& lhs, const auto& rhs) {
              if (lhs.body_a != rhs.body_a) {
                return lhs.body_a < rhs.body_a;
              }
              return lhs.body_b < rhs.body_b;
            });
  result.normalized_pairs.erase(
      std::unique(result.normalized_pairs.begin(), result.normalized_pairs.end(),
                  [](const auto& lhs, const auto& rhs) {
                    return lhs.body_a == rhs.body_a && lhs.body_b == rhs.body_b;
                  }),
      result.normalized_pairs.end());

  result.engine_status = any_unknown
                             ? core::CollisionEngineStatus::kDiagnosticIncomplete
                             : core::CollisionEngineStatus::kOk;
  result.decision = legacy_result.in_collision
                        ? (any_unknown ? core::CollisionDecisionResult::kInconclusiveFail
                                       : core::CollisionDecisionResult::kFail)
                        : core::CollisionDecisionResult::kPass;

  // Chunk 3: if a policy snapshot is installed, classify the normalized pairs
  // through a query-local evaluator. This integration is deliberately stateless
  // at diagnostic-comparator scope; cross-waypoint support-contact state belongs to
  // Chunk 5's validation coordinator.
  std::optional<core::CollisionPolicySnapshot> policy_snapshot;
  {
    std::scoped_lock lock(evaluator_mutex_);
    policy_snapshot = policy_snapshot_;
  }
  if (policy_snapshot) {
    core::CollisionPolicyEvaluator evaluator(*policy_snapshot);
    const auto active_world_revision =
        result.collision_world_revision.value_or(core::CollisionWorldRevision{});
    const bool diagnostics_complete = !any_unknown;
    auto evaluated = evaluator.classify(result.normalized_pairs,
                                        active_world_revision,
                                        diagnostics_complete);
    result.normalized_pairs_classified = std::move(evaluated.pairs);
    result.policy_revision_used =
        std::to_string(evaluator.snapshot().revision.revision_id);

    // Mirror the evaluator outcome into CollisionResult::decision. Complete
    // policy classification may allow raw engine collisions; diagnostic-
    // incomplete from the mapper remains inconclusive even when the policy
    // evaluator sees no classified pairs.
    switch (evaluated.decision.action) {
      case core::CollisionDecisionAction::kReject:
        result.decision = merge_policy_collision_decision(
            result.decision, core::CollisionDecisionResult::kFail);
        break;
      case core::CollisionDecisionAction::kInconclusive:
        result.decision = merge_policy_collision_decision(
            result.decision, core::CollisionDecisionResult::kInconclusiveFail);
        break;
      case core::CollisionDecisionAction::kWarnOnly:
        result.decision = merge_policy_collision_decision(
            result.decision, core::CollisionDecisionResult::kWarn);
        break;
      case core::CollisionDecisionAction::kDispatch:
        result.decision = merge_policy_collision_decision(
            result.decision, core::CollisionDecisionResult::kPass);
        break;
    }
  }

  // Diagnostic compare mode. If a secondary ICollisionQueryEngine is installed,
  // run it against the current CollisionWorldSnapshot and record any mismatch
  // with the legacy RoboPlan path. The secondary engine is never authoritative
  // on this adapter path.
  std::shared_ptr<core::ICollisionQueryEngine> compare_engine;
  MismatchRecordEmitter mismatch_record_emitter;
  {
    std::scoped_lock lock(compare_engine_mutex_);
    compare_engine = compare_engine_;
    mismatch_record_emitter = mismatch_record_emitter_;
  }
  if (compare_engine) {
    auto current_snapshot = snapshot();
    core::CollisionResult secondary_result;
    if (current_snapshot) {
      core::CollisionQuery compare_query = query;
      if (compare_query.collision_world_revision.revision_id == 0) {
        compare_query.collision_world_revision = current_snapshot->revision;
      }
      core::CollisionPolicySnapshot compare_policy =
          policy_snapshot.value_or(core::CollisionPolicySnapshot{});
      core::CollisionCancellationToken compare_cancel;
      secondary_result = compare_engine->check(*current_snapshot,
                                               compare_query,
                                               compare_policy,
                                               compare_cancel);
    } else {
      secondary_result.query_id = query.query_id;
      secondary_result.surface = query.surface;
      secondary_result.engine_id = compare_engine->engine_id();
      secondary_result.engine_version = compare_engine->engine_version();
      secondary_result.engine_status =
          core::CollisionEngineStatus::kDiagnosticIncomplete;
      secondary_result.decision =
          core::CollisionDecisionResult::kInconclusiveFail;
      secondary_result.raw_diagnostics["snapshot_error"] =
          "RoboPlanProjectionBackend::snapshot() returned null";
    }
    const bool mismatch =
        secondary_result.in_collision != result.in_collision ||
        secondary_result.engine_status != result.engine_status ||
        secondary_result.decision != result.decision ||
        !collision_pairs_equal(secondary_result.normalized_pairs,
                               result.normalized_pairs);
    if (mismatch) {
      std::map<std::string, std::string> details;
      details["legacy_engine_id"] = result.engine_id;
      details["legacy_in_collision"] = result.in_collision ? "true" : "false";
      details["legacy_engine_status"] =
          collision_engine_status_text(result.engine_status);
      details["legacy_decision"] = collision_decision_text(result.decision);
      details["legacy_normalized_pair_count"] =
          std::to_string(result.normalized_pairs.size());
      details["secondary_engine_id"] = secondary_result.engine_id;
      details["secondary_in_collision"] =
          secondary_result.in_collision ? "true" : "false";
      details["secondary_engine_status"] =
          collision_engine_status_text(secondary_result.engine_status);
      details["secondary_decision"] =
          collision_decision_text(secondary_result.decision);
      details["secondary_normalized_pair_count"] =
          std::to_string(secondary_result.normalized_pairs.size());
      if (result.collision_world_revision) {
        details["collision_world_revision_id"] =
            std::to_string(result.collision_world_revision->revision_id);
      }
      details["query_id"] = query.query_id;
      if (mismatch_record_emitter) {
        core::CollisionEngineMismatchRecord record;
        record.mismatch_id = result.engine_id + "_vs_" +
                              secondary_result.engine_id + "@" + query.query_id;
        record.emitted_at_ns = steady_now_ns();
        record.authoritative_engine_id = result.engine_id;
        record.authoritative_engine_version = result.engine_version;
        record.secondary_engine_id = secondary_result.engine_id;
        record.secondary_engine_version = secondary_result.engine_version;
        record.collision_world_revision = result.collision_world_revision;
        record.policy_revision = result.policy_revision_used;
        record.authoritative_normalized_pairs = result.normalized_pairs;
        record.secondary_normalized_pairs = secondary_result.normalized_pairs;
        if (!query.query_id.empty()) {
          record.query_id = query.query_id;
        }
        record.state_time_ns = query.state_time_ns;
        record.waypoint_index = query.waypoint_index;
        record.raw_diagnostics = std::move(details);
        // Default disposition is kTolerated (shadow_compare mode). Production
        // wiring overrides via a wrapping emitter; see CompareMode enum.
        record.disposition =
            core::CollisionEngineMismatchDisposition::kTolerated;
        mismatch_record_emitter(std::move(record));
      }
    }
  }

  return result;
}

core::SceneEditResultDTO RoboPlanProjectionBackend::apply_robot_base_pose(
    const core::SetRobotBasePoseRequestDTO& request) {
  std::string configured_robot_id;
  {
    std::scoped_lock mapper_lock(mapper_mutex_);
    configured_robot_id = mapper_robot_id_;
  }
  if (request.robot_id.empty() || request.robot_id != configured_robot_id) {
    auto result = core::SceneEditResultDTO{
        false,
        core::ErrorDTO{
            core::kErrorInvalidRequest,
            "Robot placement does not identify this planner projection",
            {{"reason", "robot_placement_robot_id_mismatch"},
             {"requested_robot_id", request.robot_id},
             {"projection_robot_id", configured_robot_id}}}};
    result.rollback_proven = true;
    return result;
  }

  const double quaternion_norm = std::sqrt(
      request.pose_in_world.orientation.x *
          request.pose_in_world.orientation.x +
      request.pose_in_world.orientation.y *
          request.pose_in_world.orientation.y +
      request.pose_in_world.orientation.z *
          request.pose_in_world.orientation.z +
      request.pose_in_world.orientation.w *
          request.pose_in_world.orientation.w);
  const bool finite =
      std::isfinite(request.pose_in_world.position_m.x_m) &&
      std::isfinite(request.pose_in_world.position_m.y_m) &&
      std::isfinite(request.pose_in_world.position_m.z_m) &&
      std::isfinite(request.pose_in_world.orientation.x) &&
      std::isfinite(request.pose_in_world.orientation.y) &&
      std::isfinite(request.pose_in_world.orientation.z) &&
      std::isfinite(request.pose_in_world.orientation.w) &&
      std::isfinite(quaternion_norm) && quaternion_norm > 1e-12;
  if (!finite) {
    auto result = core::SceneEditResultDTO{
        false,
        core::ErrorDTO{
            core::kErrorInvalidRequest,
            "Robot placement pose must be finite with a nonzero quaternion",
            {{"reason", "robot_placement_pose_invalid"}}}};
    result.rollback_proven = true;
    return result;
  }

  core::Pose3DTO normalized = request.pose_in_world;
  normalized.orientation.x /= quaternion_norm;
  normalized.orientation.y /= quaternion_norm;
  normalized.orientation.z /= quaternion_norm;
  normalized.orientation.w /= quaternion_norm;
  const auto proposed_world_from_robot_base = pose_to_matrix(normalized);

  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  if (world_from_robot_base_.isApprox(proposed_world_from_robot_base, 1e-12)) {
    core::SceneEditResultDTO result{true};
    result.update_status = "no_op";
    return result;
  }

  bool mutation_started = false;
  try {
    auto candidate = std::make_shared<roboplan::Scene>(*scene_);
    for (const auto& [object_id, tracked] : object_states_) {
      const Eigen::Matrix4d frame_from_object = pose_to_matrix(tracked.pose);
      Eigen::Matrix4d scene_from_object;
      if (is_world_frame(tracked.frame_id)) {
        scene_from_object =
            proposed_world_from_robot_base.inverse() * frame_from_object;
      } else {
        scene_from_object =
            parent_joint_from_frame_matrix_locked(tracked.frame_id) *
            frame_from_object;
      }
      const std::string target_frame =
          is_world_frame(tracked.frame_id) ? "universe" : tracked.frame_id;
      auto updated = candidate->updateGeometryPlacement(
          object_id, target_frame, scene_from_object);
      if (!updated.has_value()) {
        auto result = core::SceneEditResultDTO{
            false,
            core::ErrorDTO{
                core::kErrorInternalError,
                "Failed to project object after robot placement change",
                {{"reason", "robot_placement_object_projection_failed"},
                 {"object_id", object_id},
                 {"error", updated.error()}}}};
        result.rollback_proven = true;
        return result;
      }
    }

    mutation_started = true;
    *scene_ = *candidate;
    world_from_robot_base_ = proposed_world_from_robot_base;
    publish_collision_world_revision_locked(
        core::CollisionWorldMutationSource::kDeploymentLoad, {},
        "scene_projection.set_robot_base_pose");
    core::SceneEditResultDTO result{true};
    result.update_status = "geometry_changed";
    result.collision_world_revision_changed = true;
    return result;
  } catch (const std::exception& error) {
    auto result = core::SceneEditResultDTO{
        false,
        core::ErrorDTO{
            core::kErrorInternalError,
            "Robot placement projection failed",
            {{"reason", "robot_placement_projection_exception"},
                        {"exception", error.what()}}}};
    result.rollback_proven = !mutation_started;
    return result;
  }
}

std::vector<core::SceneRobotPlacementDTO>
RoboPlanProjectionBackend::get_robot_placements() const {
  std::string robot_id;
  {
    std::scoped_lock mapper_lock(mapper_mutex_);
    robot_id = mapper_robot_id_;
  }
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  return {{robot_id, matrix_to_pose(world_from_robot_base_)}};
}

core::Result<core::Pose3DTO>
RoboPlanProjectionBackend::get_object_pose_at_revision(
    const std::string& object_id, const std::string& base_frame,
    const core::SceneRevisionDTO& expected_revision) const {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  if (expected_revision.revision_id != world_revision_id_ ||
      expected_revision.content_hash != world_content_hash_) {
    return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
        core::kErrorSceneChangedDuringValidation,
        "Object pose projection does not match the committed scene revision",
        {{"reason", "scene_object_pose_revision_mismatch"},
         {"expected_revision_id",
          std::to_string(expected_revision.revision_id)},
         {"projection_revision_id", std::to_string(world_revision_id_)}}});
  }
  const auto it = object_states_.find(object_id);
  if (it == object_states_.end()) {
    return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
        core::kErrorObjectNotFound, "Object not found: " + object_id,
        {{"reason", "scene_object_pose_unavailable"}}});
  }
  try {
    const Eigen::Matrix4d frame_from_object =
        pose_to_matrix(it->second.pose);
    const Eigen::Matrix4d world_from_frame =
        frame_to_world_matrix_locked(it->second.frame_id);
    const Eigen::Matrix4d world_from_object =
        world_from_frame * frame_from_object;
    const Eigen::Matrix4d world_from_base =
        frame_to_world_matrix_locked(base_frame);
    return core::Result<core::Pose3DTO>::Ok(
        matrix_to_pose(world_from_base.inverse() * world_from_object));
  } catch (const std::exception& error) {
    return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
        core::kErrorObjectNotFound,
        "Object pose could not be resolved in the committed projection",
        {{"reason", "scene_object_pose_resolution_failed"},
         {"object_id", object_id},
         {"exception", error.what()}}});
  }
}

std::vector<RoboPlanProjectionBackend::SceneObjectSnapshot>
RoboPlanProjectionBackend::get_scene_objects() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  return scene_objects_locked();
}

std::vector<RoboPlanProjectionBackend::SceneObjectSnapshot>
RoboPlanProjectionBackend::scene_objects_locked() {
  std::vector<SceneObjectSnapshot> objects;
  objects.reserve(object_states_.size());
  for (const auto& [object_id, tracked] : object_states_) {
    objects.push_back(SceneObjectSnapshot{
        object_id,
        tracked.shape,
        tracked.pose,
        pose_in_base_from_tracked_locked(tracked, "universe"),
        tracked.frame_id,
        tracked.attached,
        tracked.touch_links,
        tracked.support_contacts,
        tracked.source_id,
        tracked.source_timestamp_ns,
        tracked.source_freshness_budget_ns,
        tracked.scene_admission_role,
        tracked.scene_admission_lease_max_age_ns,
        tracked.scene_admission_target_drift_tolerance_m,
        tracked.scene_admission_target_angular_tolerance_rad,
        tracked.scene_admission_obstacle_drift_tolerance_m,
        tracked.scene_admission_obstacle_angular_tolerance_rad,
        tracked.scene_admission_allow_runtime_dynamic_obstacle,
    });
  }
  return objects;
}

std::vector<std::pair<std::string, core::CollisionBodyId>>
RoboPlanProjectionBackend::robot_body_mappings_locked(
    const std::string& robot_id) {
  std::vector<std::pair<std::string, core::CollisionBodyId>> mappings;
  if (!scene_) {
    return mappings;
  }

  const auto& model = scene_->getModel();
  const auto& collision_model = scene_->getCollisionModel();
  std::map<std::string, int> next_collision_index_by_link;
  std::set<std::string> emitted_raw_names;
  std::set<std::string> scene_object_ids;
  for (const auto& [object_id, tracked] : object_states_) {
    static_cast<void>(tracked);
    scene_object_ids.insert(object_id);
  }

  for (const auto& geometry_object : collision_model.geometryObjects) {
    if (scene_object_ids.find(geometry_object.name) != scene_object_ids.end()) {
      continue;
    }
    if (geometry_object.parentFrame >= model.frames.size()) {
      continue;
    }
    const std::string link_name = model.frames[geometry_object.parentFrame].name;
    if (link_name.empty() || is_world_frame(link_name) ||
        scene_object_ids.find(link_name) != scene_object_ids.end()) {
      continue;
    }

    const int collision_index = next_collision_index_by_link[link_name]++;
    const auto body_id = RoboPlanCollisionBodyIdMapper::make_robot_link_id(
        robot_id, link_name, collision_index);

    if (emitted_raw_names.insert(geometry_object.name).second) {
      mappings.emplace_back(geometry_object.name, body_id);
    }
    // RoboPlan diagnostics have historically used either geometry-object names
    // or frame/link names depending on the underlying version and query path.
    // Map the frame name to the first collision body on that link so common
    // diagnostics still normalize. Geometry names above retain per-shape
    // precision when available.
    if (collision_index == 0 && emitted_raw_names.insert(link_name).second) {
      mappings.emplace_back(link_name, body_id);
    }
  }

  std::sort(mappings.begin(), mappings.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) {
      return a.first < b.first;
    }
    return a.second < b.second;
  });
  return mappings;
}

std::vector<std::string> RoboPlanProjectionBackend::object_body_ids_for_state_locked(
    const std::string& object_id,
    const TrackedObjectState& state) const {
  std::string robot_id;
  {
    std::scoped_lock map_lock(mapper_mutex_);
    robot_id = mapper_robot_id_;
  }
  constexpr int kShapeIndex = 0;
  if (state.attached) {
    return {RoboPlanCollisionBodyIdMapper::make_attached_object_id(
        robot_id, object_id, kShapeIndex)};
  }
  return {RoboPlanCollisionBodyIdMapper::make_detached_object_id(
      object_id, kShapeIndex)};
}

std::vector<std::string>
RoboPlanProjectionBackend::object_body_ids_for_transition_locked(
    const std::string& object_id,
    const TrackedObjectState& before,
    const TrackedObjectState& after) const {
  auto affected = object_body_ids_for_state_locked(object_id, before);
  auto after_ids = object_body_ids_for_state_locked(object_id, after);
  affected.insert(affected.end(), after_ids.begin(), after_ids.end());
  std::sort(affected.begin(), affected.end());
  affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
  return affected;
}

std::vector<core::CollisionWorldBody>
RoboPlanProjectionBackend::robot_snapshot_bodies_locked(
    const std::string& robot_id) const {
  std::vector<core::CollisionWorldBody> bodies;
  if (!scene_) {
    return bodies;
  }

  const auto& model = scene_->getModel();
  const auto& collision_model = scene_->getCollisionModel();
  std::set<std::string> scene_object_ids;
  for (const auto& [object_id, tracked] : object_states_) {
    static_cast<void>(tracked);
    scene_object_ids.insert(object_id);
  }

  std::map<std::string, int> next_collision_index_by_link;
  const auto& positions = scene_->getCurrentJointPositions();
  Eigen::VectorXd q(positions.size());
  for (Eigen::Index i = 0; i < positions.size(); ++i) {
    q[i] = positions[i];
  }

  for (const auto& geometry_object : collision_model.geometryObjects) {
    if (scene_object_ids.find(geometry_object.name) != scene_object_ids.end()) {
      continue;
    }
    if (geometry_object.parentFrame >= model.frames.size()) {
      continue;
    }
    const auto& frame = model.frames[geometry_object.parentFrame];
    const std::string link_name = frame.name;
    if (link_name.empty() || is_world_frame(link_name) ||
        scene_object_ids.find(link_name) != scene_object_ids.end()) {
      continue;
    }

    const int collision_index = next_collision_index_by_link[link_name]++;
    const auto body_id = RoboPlanCollisionBodyIdMapper::make_robot_link_id(
        robot_id, link_name, collision_index);

    Eigen::Matrix4d t_world_link = Eigen::Matrix4d::Identity();
    try {
      t_world_link = world_from_robot_base_ * scene_->forwardKinematics(q, link_name);
    } catch (const std::exception&) {
      // Fail closed later through the snapshot builder by publishing a
      // deterministic, unsupported compound descriptor instead of silently
      // omitting the robot body.
      t_world_link = world_from_robot_base_;
    }

    const Eigen::Matrix4d t_joint_link =
        frame.placement.toHomogeneousMatrix();
    const Eigen::Matrix4d t_link_geometry =
        t_joint_link.inverse() * geometry_object.placement.toHomogeneousMatrix();

    core::CollisionWorldBody body;
    body.body_id = body_id;
    body.owner_kind = core::CollisionOwnerKind::kRobotLink;
    body.frame_id = "universe";
    body.pose = matrix_to_pose(t_world_link);
    body.geometries.push_back(descriptor_from_coal_geometry(
        body_id, geometry_object, matrix_to_pose(t_link_geometry)));
    bodies.push_back(std::move(body));
  }

  return bodies;
}

core::CollisionGeometryDescriptor
RoboPlanProjectionBackend::geometry_descriptor_for_shape_locked(
    const core::CollisionBodyId& body_id,
    core::CollisionOwnerKind owner_kind,
    const core::CollisionShapeDTO& shape) const {
  core::CollisionGeometryDescriptor descriptor;
  descriptor.body_id = body_id;
  descriptor.owner_kind = owner_kind;
  descriptor.local_pose.orientation.w = 1.0;
  descriptor.quality_labels = {"scene_object"};

  std::visit(
      [&](const auto& concrete) {
        using ShapeT = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<ShapeT, core::BoxShapeDTO>) {
          descriptor.geometry_kind = core::CollisionGeometryKind::kBox;
          descriptor.box_dimensions_m = concrete.dimensions_m;
          add_common_quality_label(descriptor, "primitive");
        } else if constexpr (std::is_same_v<ShapeT, core::SphereShapeDTO>) {
          descriptor.geometry_kind = core::CollisionGeometryKind::kSphere;
          descriptor.sphere_radius_m = concrete.radius_m;
          add_common_quality_label(descriptor, "primitive");
        } else if constexpr (std::is_same_v<ShapeT, core::CylinderShapeDTO>) {
          descriptor.geometry_kind = core::CollisionGeometryKind::kCylinder;
          descriptor.cylinder_radius_m = concrete.radius_m;
          descriptor.cylinder_height_m = concrete.height_m;
          add_common_quality_label(descriptor, "primitive");
        } else if constexpr (std::is_same_v<ShapeT, core::MeshShapeDTO>) {
          descriptor.geometry_kind = core::CollisionGeometryKind::kTriangleMesh;
          descriptor.source_uri = concrete.resource_uri;
          descriptor.source_hash = effective_mesh_source_hash(concrete);
          descriptor.mesh_scale = core::Vector3DTO{
              concrete.scale.x_m == 0.0 ? 1.0 : concrete.scale.x_m,
              concrete.scale.y_m == 0.0 ? 1.0 : concrete.scale.y_m,
              concrete.scale.z_m == 0.0 ? 1.0 : concrete.scale.z_m};
          add_common_quality_label(descriptor, "mesh");
          add_common_quality_label(descriptor, "collision_mesh");
        }
      },
      shape);

  return descriptor;
}

std::vector<core::CollisionWorldBody>
RoboPlanProjectionBackend::object_snapshot_bodies_locked(
    const std::string& robot_id) const {
  std::vector<core::CollisionWorldBody> bodies;
  bodies.reserve(object_states_.size());
  std::set<std::string> live_body_ids;

  for (const auto& [object_id, tracked] : object_states_) {
    const auto body_ids = object_body_ids_for_state_locked(object_id, tracked);
    if (body_ids.empty()) {
      continue;
    }
    core::CollisionWorldBody body;
    body.body_id = body_ids.front();
    body.owner_kind = tracked.attached
                          ? core::CollisionOwnerKind::kAttachedObject
                          : core::CollisionOwnerKind::kDetachedObject;
    if (tracked.attached) {
      const auto parent_link = robot_link_parent_body_id(robot_id, tracked.frame_id);
      body.parent_link = parent_link;
      body.frame_id = parent_link;
      body.pose = tracked.pose;
    } else {
      body.frame_id = "universe";
      body.pose = pose_in_base_from_tracked_locked(tracked, "universe");
    }
    body.source_id = tracked.source_id;
    body.source_timestamp_ns = tracked.source_timestamp_ns;
    body.source_freshness_budget_ns = tracked.source_freshness_budget_ns;
    body.geometries.push_back(geometry_descriptor_for_shape_locked(
        body.body_id, body.owner_kind, tracked.shape));
    const auto fingerprint = collision_world_body_cache_fingerprint(body);
    const auto cache_it = object_body_hash_cache_.find(body.body_id);
    if (cache_it != object_body_hash_cache_.end() &&
        cache_it->second.first == fingerprint) {
      body.body_content_hash = cache_it->second.second;
    } else {
      body.body_content_hash =
          core::compute_collision_world_body_content_hash(body);
      object_body_hash_cache_[body.body_id] = {
          fingerprint,
          body.body_content_hash,
      };
    }
    live_body_ids.insert(body.body_id);
    bodies.push_back(std::move(body));
  }

  for (auto it = object_body_hash_cache_.begin();
       it != object_body_hash_cache_.end();) {
    if (live_body_ids.find(it->first) == live_body_ids.end()) {
      it = object_body_hash_cache_.erase(it);
    } else {
      ++it;
    }
  }

  return bodies;
}

std::string RoboPlanProjectionBackend::robot_model_frame_id_locked() const {
  if (!scene_) {
    return "base_link";
  }
  const auto& model = scene_->getModel();
  for (const auto& frame : model.frames) {
    if (frame.name.empty() || is_world_frame(frame.name)) {
      continue;
    }
    if (frame.type == pinocchio::BODY && frame.parentJoint == 0) {
      return frame.name;
    }
  }
  if (model.existFrame("base_link")) {
    return "base_link";
  }
  for (const auto& frame : model.frames) {
    if (!frame.name.empty() && !is_world_frame(frame.name) &&
        frame.type == pinocchio::BODY) {
      return frame.name;
    }
  }
  return "base_link";
}

Eigen::Matrix4d RoboPlanProjectionBackend::robot_model_frame_world_matrix_locked(
    const std::string& model_frame_id) const {
  if (is_world_frame(model_frame_id)) {
    return Eigen::Matrix4d::Identity();
  }
  const auto& positions = scene_->getCurrentJointPositions();
  Eigen::VectorXd q(positions.size());
  for (Eigen::Index i = 0; i < positions.size(); ++i) {
    q[i] = positions[i];
  }
  return world_from_robot_base_ * scene_->forwardKinematics(q, model_frame_id);
}

core::CollisionWorldSnapshotPtr
RoboPlanProjectionBackend::build_collision_world_snapshot_locked() const {
  std::string robot_id;
  {
    std::scoped_lock map_lock(mapper_mutex_);
    robot_id = mapper_robot_id_;
  }

  core::CollisionWorldSnapshotBuilder builder;
  builder.set_publisher_id("roboplan_context")
      .set_revision_id(world_revision_id_)
      .set_emitted_at_ns(static_cast<std::int64_t>(steady_now_ns()));

  core::CollisionRobotModelFrame robot_frame;
  robot_frame.robot_id = robot_id;
  robot_frame.model_frame_id = robot_model_frame_id_locked();
  robot_frame.world_pose =
      matrix_to_pose(robot_model_frame_world_matrix_locked(robot_frame.model_frame_id));
  builder.add_robot_model_frame(std::move(robot_frame));

  for (auto& body : robot_snapshot_bodies_locked(robot_id)) {
    builder.add_body(std::move(body));
  }
  for (auto& body : object_snapshot_bodies_locked(robot_id)) {
    builder.add_body(std::move(body));
  }

  auto result = builder.build_shared();
  if (!result.ok()) {
    return nullptr;
  }
  return std::move(result.value());
}

std::optional<core::CollisionPolicySnapshot>
RoboPlanProjectionBackend::collision_policy_snapshot() const {
  std::string robot_id;
  {
    std::scoped_lock map_lock(mapper_mutex_);
    robot_id = mapper_robot_id_;
  }

  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  return build_collision_policy_snapshot_locked(robot_id);
}

core::CollisionWorldPolicySnapshotBundle
RoboPlanProjectionBackend::collision_world_policy_snapshot_bundle() const {
  std::string robot_id;
  {
    std::scoped_lock map_lock(mapper_mutex_);
    robot_id = mapper_robot_id_;
  }

  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  return current_collision_world_policy_snapshot_bundle_locked(robot_id);
}

core::CollisionWorldPolicySnapshotBundle
RoboPlanProjectionBackend::current_collision_world_policy_snapshot_bundle_locked(
    const std::string& robot_id) const {
  if (!snapshot_cache_ ||
      snapshot_cache_revision_id_ != world_revision_id_ ||
      snapshot_cache_content_hash_ != world_content_hash_) {
    auto next = build_collision_world_snapshot_locked();
    if (!next) {
      snapshot_cache_.reset();
      snapshot_cache_revision_id_ = 0;
      snapshot_cache_content_hash_.clear();
      return {};
    }
    snapshot_cache_revision_id_ = world_revision_id_;
    snapshot_cache_content_hash_ = world_content_hash_;
    snapshot_cache_ = std::move(next);
  }
  core::CollisionWorldPolicySnapshotBundle bundle;
  bundle.world = snapshot_cache_;
  bundle.policy = build_collision_policy_snapshot_locked(robot_id);
  bundle.body_freshness = runtime_body_freshness_locked();
  return bundle;
}

std::optional<core::CollisionWorldLeaseInventory>
RoboPlanProjectionBackend::current_scene_snapshot_lease_inventory() const {
  std::string robot_id;
  {
    std::scoped_lock map_lock(mapper_mutex_);
    robot_id = mapper_robot_id_;
  }

  const auto now_ns = static_cast<std::int64_t>(steady_now_ns());
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  auto inventory =
      build_scene_snapshot_lease_inventory_locked(robot_id, {}, now_ns);
  if (!inventory.bundle.world || !inventory.bundle.policy.has_value()) {
    return std::nullopt;
  }
  return inventory;
}

core::RuntimeCollisionQueryBundlePtr
RoboPlanProjectionBackend::published_runtime_collision_query_bundle() const {
  return published_runtime_bundle_.load(std::memory_order_acquire);
}

bool RoboPlanProjectionBackend::publish_runtime_query_bundle_locked(
    core::RuntimeCollisionQueryBundlePtr bundle) {
  for (auto& retired : retired_runtime_bundles_) {
    if (retired && retired.use_count() == 1) {
      retired.reset();
    }
  }
  if (backpressured_runtime_bundle_ &&
      backpressured_runtime_bundle_.use_count() == 1) {
    backpressured_runtime_bundle_.reset();
  }

  auto current = published_runtime_bundle_.load(std::memory_order_relaxed);
  if (!current) {
    // A failed replacement has already pinned the displaced bundle in the
    // dedicated NRT slot. Refill the now-empty atomic slot without disturbing
    // that owner so an explicit authority rebuild can recover even while the
    // previous committed publication is still referenced. A later replacement
    // remains bounded by the guard below until a retirement slot is reclaimed.
    published_runtime_bundle_.store(std::move(bundle),
                                    std::memory_order_release);
    ++runtime_bundle_publish_count_;
    return true;
  }

  auto available = retired_runtime_bundles_.end();
  available = std::find(retired_runtime_bundles_.begin(),
                        retired_runtime_bundles_.end(), nullptr);
  if (available == retired_runtime_bundles_.end()) {
    if (backpressured_runtime_bundle_) {
      // Keep the current coherent publication visible. Overwriting the pinned
      // displaced bundle could make a RuntimeSafety reader the last owner and
      // move heavy destruction onto its tick.
      ++runtime_bundle_publish_backpressure_count_;
      return false;
    }
    backpressured_runtime_bundle_ =
        published_runtime_bundle_.exchange(nullptr,
                                           std::memory_order_acq_rel);
    ++runtime_bundle_publish_backpressure_count_;
    return false;
  }

  auto previous = published_runtime_bundle_.exchange(std::move(bundle),
                                                     std::memory_order_acq_rel);
  if (previous) {
    *available = std::move(previous);
  }
  ++runtime_bundle_publish_count_;
  return true;
}

std::vector<core::CollisionWorldBodyFreshness>
RoboPlanProjectionBackend::runtime_body_freshness_locked() const {
  std::vector<core::CollisionWorldBodyFreshness> freshness;
  for (const auto& [object_id, tracked] : object_states_) {
    if (!tracked.source_freshness_budget_ns.has_value()) {
      continue;
    }
    for (const auto& body_id :
         object_body_ids_for_state_locked(object_id, tracked)) {
      core::CollisionWorldBodyFreshness entry;
      entry.body_id = body_id;
      entry.source_id = tracked.source_id;
      entry.source_timestamp_ns = tracked.source_timestamp_ns;
      entry.source_freshness_budget_ns = tracked.source_freshness_budget_ns;
      freshness.push_back(std::move(entry));
    }
  }
  std::sort(freshness.begin(), freshness.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.body_id < rhs.body_id;
            });
  return freshness;
}

void RoboPlanProjectionBackend::publish_runtime_body_freshness_locked() {
  auto freshness = runtime_body_freshness_locked();
  auto current = published_runtime_bundle_.load(std::memory_order_acquire);
  // During bounded-retirement backpressure the public slot is intentionally
  // null, but the displaced coherent bundle remains pinned under scene_mutex_.
  // Derive a freshness-only successor from that bundle so the first recovery
  // publication cannot lose world/policy/index ownership.
  const auto& base = current ? current : backpressured_runtime_bundle_;
  auto next = std::make_shared<core::RuntimeCollisionQueryBundle>();
  if (base) {
    next->world = base->world;
    next->policy = base->policy;
    next->policy_index = base->policy_index;
  }
  next->body_freshness = std::move(freshness);
  next->lookup_order_valid =
      next->world && core::runtime_collision_lookup_order_is_valid(
                         *next->world, next->body_freshness);
  next->epoch = ++published_runtime_bundle_epoch_;
  (void)publish_runtime_query_bundle_locked(std::move(next));
}

RoboPlanProjectionBackend::PoseUpdateClassification
RoboPlanProjectionBackend::classify_pose_update_locked(
    const std::string& object_id,
    const TrackedObjectState& previous_state,
    const TrackedObjectState& new_state,
    const std::string& robot_id) const {
  const bool geometry_same =
      pose_equal(previous_state.pose, new_state.pose) &&
      object_revision_token_locked(object_id, previous_state, robot_id) ==
          object_revision_token_locked(object_id, new_state, robot_id);
  if (!geometry_same) {
    return PoseUpdateClassification::kGeometryChanged;
  }
  if (previous_state.source_timestamp_ns != new_state.source_timestamp_ns) {
    return PoseUpdateClassification::kFreshnessOnly;
  }
  return PoseUpdateClassification::kNoOp;
}

core::CollisionWorldLeaseInventory
RoboPlanProjectionBackend::build_scene_snapshot_lease_inventory_locked(
    const std::string& robot_id,
    std::string retention_id,
    std::int64_t captured_at_ns) const {
  core::CollisionWorldLeaseInventory inventory;
  inventory.retention_id = std::move(retention_id);
  inventory.captured_at_ns = captured_at_ns;
  inventory.bundle = current_collision_world_policy_snapshot_bundle_locked(robot_id);
  inventory.topology_epoch = topology_epoch_;
  inventory.policy_epoch = policy_epoch_;
  inventory.live_objects = scene_snapshot_live_objects_locked(robot_id);
  return inventory;
}

std::vector<core::SceneSnapshotLiveObject>
RoboPlanProjectionBackend::scene_snapshot_live_objects_locked(
    const std::string& robot_id) const {
  std::vector<core::SceneSnapshotLiveObject> live_objects;
  live_objects.reserve(object_states_.size());
  for (const auto& [object_id, tracked] : object_states_) {
    core::SceneSnapshotLiveObject live;
    live.object_id = object_id;
    live.collision_body_ids = object_body_ids_for_state_locked(object_id, tracked);
    live.object_revision_token =
        object_revision_token_locked(object_id, tracked, robot_id);
    live.pose = pose_in_base_from_tracked_locked(tracked, "universe");
    live.declared_role = tracked.scene_admission_role;
    live.source_id = tracked.source_id;
    live.source_timestamp_ns = tracked.source_timestamp_ns;
    live.source_freshness_budget_ns = tracked.source_freshness_budget_ns;
    live.lease_max_age_ns = tracked.scene_admission_lease_max_age_ns;
    live.target_drift_tolerance_m =
        tracked.scene_admission_target_drift_tolerance_m;
    live.target_angular_tolerance_rad =
        tracked.scene_admission_target_angular_tolerance_rad;
    live.obstacle_drift_tolerance_m =
        tracked.scene_admission_obstacle_drift_tolerance_m;
    live.obstacle_angular_tolerance_rad =
        tracked.scene_admission_obstacle_angular_tolerance_rad;
    live.runtime_dynamic_obstacle_allowed =
        tracked.scene_admission_allow_runtime_dynamic_obstacle;
    live_objects.push_back(std::move(live));
  }
  return live_objects;
}

std::string RoboPlanProjectionBackend::object_revision_token_locked(
    const std::string& object_id,
    const TrackedObjectState& state,
    const std::string& robot_id) const {
  std::vector<std::string> entries;
  entries.push_back("object_id=" + object_id);
  entries.push_back("robot_id=" + robot_id);
  entries.push_back("frame_id=" + state.frame_id);
  entries.push_back("attached=" + std::string(state.attached ? "1" : "0"));
  for (const auto& body_id : object_body_ids_for_state_locked(object_id, state)) {
    entries.push_back("body_id=" + body_id);
  }
  append_shape_entries(entries, "shape", state.shape);

  std::vector<std::string> touch_links = state.touch_links;
  std::sort(touch_links.begin(), touch_links.end());
  for (const auto& link : touch_links) {
    entries.push_back("touch_link=" + link);
  }

  std::vector<std::string> support_contacts;
  support_contacts.reserve(state.support_contacts.size());
  for (const auto& contact : state.support_contacts) {
    support_contacts.push_back(
        contact.support_object_id + ":" +
        std::to_string(static_cast<int>(contact.mode)) + ":" +
        (contact.enforce_max_start_penetration ? "1" : "0") + ":" +
        stable_double(contact.max_start_penetration_m) + ":" +
        stable_double(contact.release_tolerance_m) + ":" +
        stable_double(contact.min_release_progress_m) + ":" +
        stable_double(contact.min_separation_clearance_m) + ":" +
        (contact.require_separation_by_end ? "1" : "0") + ":" +
        (contact.require_strictly_decreasing_penetration ? "1" : "0") +
        ":" + (contact.allow_recontact ? "1" : "0"));
  }
  std::sort(support_contacts.begin(), support_contacts.end());
  for (const auto& contact : support_contacts) {
    entries.push_back("support_contact=" + contact);
  }

  entries.push_back(
      "source_id=" + state.source_id.value_or(""));
  entries.push_back(
      "source_freshness_budget_set=" +
      std::string(state.source_freshness_budget_ns.has_value() ? "1" : "0"));
  if (state.source_freshness_budget_ns.has_value()) {
    entries.push_back(
        "source_freshness_budget_ns=" +
        std::to_string(*state.source_freshness_budget_ns));
  }
  entries.push_back(
      "scene_admission_role=" +
      std::string(state.scene_admission_role.has_value()
                      ? core::to_string(*state.scene_admission_role)
                      : ""));
  entries.push_back(
      "scene_admission_lease_max_age_ns=" +
      (state.scene_admission_lease_max_age_ns.has_value()
           ? std::to_string(*state.scene_admission_lease_max_age_ns)
           : ""));
  entries.push_back(
      "scene_admission_target_drift_tolerance_m=" +
      (state.scene_admission_target_drift_tolerance_m.has_value()
           ? stable_double(*state.scene_admission_target_drift_tolerance_m)
           : ""));
  entries.push_back(
      "scene_admission_target_angular_tolerance_rad=" +
      (state.scene_admission_target_angular_tolerance_rad.has_value()
           ? stable_double(*state.scene_admission_target_angular_tolerance_rad)
           : ""));
  entries.push_back(
      "scene_admission_obstacle_drift_tolerance_m=" +
      (state.scene_admission_obstacle_drift_tolerance_m.has_value()
           ? stable_double(*state.scene_admission_obstacle_drift_tolerance_m)
           : ""));
  entries.push_back(
      "scene_admission_obstacle_angular_tolerance_rad=" +
      (state.scene_admission_obstacle_angular_tolerance_rad.has_value()
           ? stable_double(*state.scene_admission_obstacle_angular_tolerance_rad)
           : ""));
  entries.push_back(
      "scene_admission_allow_runtime_dynamic_obstacle=" +
      std::string(state.scene_admission_allow_runtime_dynamic_obstacle ? "1" : "0"));

  std::sort(entries.begin(), entries.end());
  core::ContentHasher h;
  h.mix("manyforge_roboplan/scene_snapshot_object_revision_token/v1");
  for (const auto& entry : entries) {
    h.mix(entry);
  }
  return h.finalize();
}

core::CollisionPolicySnapshot
RoboPlanProjectionBackend::build_collision_policy_snapshot_locked(
    const std::string& robot_id) const {
  core::CollisionPolicySnapshot policy;
  {
    std::scoped_lock eval_lock(evaluator_mutex_);
    if (policy_snapshot_) {
      policy = *policy_snapshot_;
    }
  }

  auto add_pair = [](std::vector<core::CollisionPairId>& pairs,
                     std::string a,
                     std::string b) {
    if (a.empty() || b.empty() || a == b) {
      return;
    }
    if (b < a) {
      std::swap(a, b);
    }
    pairs.push_back(core::CollisionPairId{std::move(a), std::move(b)});
  };
  auto sort_unique_pairs = [](std::vector<core::CollisionPairId>& pairs) {
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
  };

  for (const auto& pair : disabled_static_collision_pairs_) {
    const auto a = object_states_.find(pair.first);
    const auto b = object_states_.find(pair.second);
    if (a == object_states_.end() || b == object_states_.end()) {
      continue;
    }
    if (a->second.attached || b->second.attached) {
      continue;
    }
    for (const auto& body_a :
         object_body_ids_for_state_locked(a->first, a->second)) {
      for (const auto& body_b :
           object_body_ids_for_state_locked(b->first, b->second)) {
        add_pair(policy.allowed_static_static_pairs, body_a, body_b);
      }
    }
  }

  const auto robot_bodies = robot_snapshot_bodies_locked(robot_id);
  std::set<std::string> group_joint_names;
  if (const auto maybe_group_info = scene_->getJointGroupInfo(group_name_);
      maybe_group_info.has_value()) {
    group_joint_names.insert(maybe_group_info->joint_names.begin(),
                             maybe_group_info->joint_names.end());
  }
  std::vector<core::CollisionBodyId> static_robot_bodies;
  const auto& model = scene_->getModel();
  for (const auto& robot_body : robot_bodies) {
    auto link = robot_link_from_collision_body_id(robot_body.body_id, robot_id);
    if (!link) {
      continue;
    }
    if (link_is_static_wrt_group(model, *link, group_joint_names)) {
      static_robot_bodies.push_back(robot_body.body_id);
    }
  }

  // Fixed robot-base geometry is world-static for this trajectory. Contacts
  // between that geometry and detached scene objects are scene-health data, not
  // motion validation blockers. Moving robot links remain fully checked.
  for (const auto& robot_body_id : static_robot_bodies) {
    for (const auto& [object_id, tracked] : object_states_) {
      if (tracked.attached) {
        continue;
      }
      for (const auto& object_body_id :
           object_body_ids_for_state_locked(object_id, tracked)) {
        add_pair(policy.allowed_static_static_pairs,
                 robot_body_id,
                 object_body_id);
      }
    }
  }

  for (const auto& [object_id, tracked] : object_states_) {
    if (!tracked.attached || tracked.touch_links.empty()) {
      continue;
    }
    const auto object_body_ids =
        object_body_ids_for_state_locked(object_id, tracked);
    for (const auto& touch_link : tracked.touch_links) {
      for (const auto& robot_body : robot_bodies) {
        if (!robot_collision_body_id_matches_link(robot_body.body_id,
                                                  robot_id,
                                                  touch_link)) {
          continue;
        }
        for (const auto& object_body_id : object_body_ids) {
          add_pair(policy.allowed_touch_link_pairs,
                   object_body_id,
                   robot_body.body_id);
        }
      }
    }
  }

  for (const auto& pair : disabled_support_contact_pairs_) {
    const auto a = object_states_.find(pair.first);
    const auto b = object_states_.find(pair.second);
    if (a == object_states_.end() || b == object_states_.end()) {
      continue;
    }
    std::optional<core::SupportContactDTO> configured =
        support_contact_policy_for_pair_locked(pair);
    for (const auto& body_a :
         object_body_ids_for_state_locked(a->first, a->second)) {
      for (const auto& body_b :
           object_body_ids_for_state_locked(b->first, b->second)) {
        core::SupportContactPolicyEntry entry;
        if (body_b < body_a) {
          entry.pair = core::CollisionPairId{body_b, body_a};
        } else {
          entry.pair = core::CollisionPairId{body_a, body_b};
        }
        if (configured) {
          entry.mode = configured->mode;
          entry.enforce_max_start_penetration =
              configured->enforce_max_start_penetration;
          entry.max_start_penetration_m =
              configured->max_start_penetration_m;
          entry.release_tolerance_m = configured->release_tolerance_m;
          entry.min_release_progress_m =
              configured->min_release_progress_m;
          entry.min_separation_clearance_m =
              configured->min_separation_clearance_m;
          entry.require_separation_by_end =
              configured->require_separation_by_end;
          entry.require_strictly_decreasing_penetration =
              configured->require_strictly_decreasing_penetration;
          entry.allow_recontact = configured->allow_recontact;
        }
        policy.support_contacts.push_back(std::move(entry));
      }
    }
  }
  if (!policy.support_contacts.empty()) {
    // Contact-release is a numeric safety contract. It is never valid to
    // evaluate support contacts through a degraded body-pair-only policy: the
    // ManyForge engine must provide signed clearance / penetration diagnostics.
    policy.require_full_diagnostics = true;
  }

  sort_unique_pairs(policy.allowed_static_static_pairs);
  sort_unique_pairs(policy.allowed_touch_link_pairs);
  std::sort(policy.support_contacts.begin(),
            policy.support_contacts.end(),
            [](const auto& lhs, const auto& rhs) {
              if (lhs.pair != rhs.pair) {
                return lhs.pair < rhs.pair;
              }
              if (lhs.mode != rhs.mode) {
                return static_cast<int>(lhs.mode) < static_cast<int>(rhs.mode);
              }
              if (lhs.enforce_max_start_penetration !=
                  rhs.enforce_max_start_penetration) {
                return lhs.enforce_max_start_penetration <
                       rhs.enforce_max_start_penetration;
              }
              if (lhs.max_start_penetration_m !=
                  rhs.max_start_penetration_m) {
                return lhs.max_start_penetration_m <
                       rhs.max_start_penetration_m;
              }
              if (lhs.release_tolerance_m != rhs.release_tolerance_m) {
                return lhs.release_tolerance_m < rhs.release_tolerance_m;
              }
              if (lhs.min_release_progress_m !=
                  rhs.min_release_progress_m) {
                return lhs.min_release_progress_m <
                       rhs.min_release_progress_m;
              }
              if (lhs.min_separation_clearance_m !=
                  rhs.min_separation_clearance_m) {
                return lhs.min_separation_clearance_m <
                       rhs.min_separation_clearance_m;
              }
              if (lhs.require_separation_by_end !=
                  rhs.require_separation_by_end) {
                return lhs.require_separation_by_end <
                       rhs.require_separation_by_end;
              }
              if (lhs.require_strictly_decreasing_penetration !=
                  rhs.require_strictly_decreasing_penetration) {
                return lhs.require_strictly_decreasing_penetration <
                       rhs.require_strictly_decreasing_penetration;
              }
              return lhs.allow_recontact < rhs.allow_recontact;
            });
  policy.support_contacts.erase(
      std::unique(policy.support_contacts.begin(),
                  policy.support_contacts.end(),
                  [](const auto& lhs, const auto& rhs) {
                    return lhs.pair == rhs.pair &&
                           lhs.mode == rhs.mode &&
                           lhs.enforce_max_start_penetration ==
                               rhs.enforce_max_start_penetration &&
                           lhs.max_start_penetration_m ==
                               rhs.max_start_penetration_m &&
                           lhs.release_tolerance_m ==
                               rhs.release_tolerance_m &&
                           lhs.min_release_progress_m ==
                               rhs.min_release_progress_m &&
                           lhs.min_separation_clearance_m ==
                               rhs.min_separation_clearance_m &&
                           lhs.require_separation_by_end ==
                               rhs.require_separation_by_end &&
                           lhs.require_strictly_decreasing_penetration ==
                               rhs.require_strictly_decreasing_penetration &&
                           lhs.allow_recontact == rhs.allow_recontact;
                  }),
      policy.support_contacts.end());

  core::ContentHasher h;
  h.mix("manyforge_roboplan/collision_policy_snapshot")
      .mix(robot_id)
      .mix(policy.policy_id)
      .mix(static_cast<std::int64_t>(
          policy.require_full_diagnostics ? 1 : 0));
  for (const auto& pair : policy.allowed_static_static_pairs) {
    h.mix("static").mix(pair.body_a).mix(pair.body_b);
  }
  for (const auto& pair : policy.allowed_touch_link_pairs) {
    h.mix("touch").mix(pair.body_a).mix(pair.body_b);
  }
  for (const auto& entry : policy.support_contacts) {
    h.mix("support")
        .mix(entry.pair.body_a)
        .mix(entry.pair.body_b)
        .mix(static_cast<std::int64_t>(entry.mode))
        .mix(static_cast<std::int64_t>(
            entry.enforce_max_start_penetration ? 1 : 0))
        .mix(entry.max_start_penetration_m)
        .mix(entry.release_tolerance_m)
        .mix(entry.min_release_progress_m)
        .mix(entry.min_separation_clearance_m)
        .mix(static_cast<std::int64_t>(
            entry.require_separation_by_end ? 1 : 0))
        .mix(static_cast<std::int64_t>(
            entry.require_strictly_decreasing_penetration ? 1 : 0))
        .mix(static_cast<std::int64_t>(entry.allow_recontact ? 1 : 0));
  }
  for (const auto& entry : policy.contact_grasps) {
    h.mix("contact_grasp")
        .mix(entry.policy_id)
        .mix(entry.selected_object_id)
        .mix(entry.max_penetration_m)
        .mix(entry.terminal_window_start_fraction);
    for (const auto& link : entry.allowed_robot_links) {
      h.mix("link").mix(link);
    }
  }
  policy.revision.revision_id = policy_epoch_;
  policy.revision.content_hash = h.finalize();
  return policy;
}

std::string RoboPlanProjectionBackend::compute_collision_world_content_hash_locked(
    const std::string& robot_id) {
  std::vector<std::string> entries;

  auto robot_mappings = robot_body_mappings_locked(robot_id);
  for (const auto& [raw_name, body_id] : robot_mappings) {
    entries.push_back("robot.raw=" + raw_name + ".body=" + body_id);
  }

  for (const auto& [object_id, tracked] : object_states_) {
    const std::string prefix = "object." + object_id;
    entries.push_back(prefix + ".frame=" + tracked.frame_id);
    entries.push_back(prefix + ".attached=" + (tracked.attached ? "1" : "0"));
    for (const auto& body_id :
         object_body_ids_for_state_locked(object_id, tracked)) {
      entries.push_back(prefix + ".body=" + body_id);
    }
    append_pose_entries(entries, prefix + ".pose", tracked.pose);
    append_shape_entries(entries, prefix, tracked.shape);

    std::vector<std::string> touch_links = tracked.touch_links;
    std::sort(touch_links.begin(), touch_links.end());
    for (const auto& link : touch_links) {
      entries.push_back(prefix + ".touch_link=" + link);
    }

    std::vector<std::string> support_contacts;
    support_contacts.reserve(tracked.support_contacts.size());
    for (const auto& contact : tracked.support_contacts) {
      support_contacts.push_back(
          contact.support_object_id + ":" +
          std::to_string(static_cast<int>(contact.mode)) + ":" +
          (contact.enforce_max_start_penetration ? "1" : "0") + ":" +
          stable_double(contact.max_start_penetration_m) + ":" +
          stable_double(contact.release_tolerance_m) + ":" +
          stable_double(contact.min_release_progress_m) + ":" +
          stable_double(contact.min_separation_clearance_m) + ":" +
          (contact.require_separation_by_end ? "1" : "0") + ":" +
          (contact.require_strictly_decreasing_penetration ? "1" : "0") +
          ":" + (contact.allow_recontact ? "1" : "0"));
    }
    std::sort(support_contacts.begin(), support_contacts.end());
    for (const auto& contact : support_contacts) {
      entries.push_back(prefix + ".support_contact=" + contact);
    }
  }

  for (const auto& pair : disabled_static_collision_pairs_) {
    entries.push_back("policy.disabled_static=" + pair.first + "|" + pair.second);
  }
  for (const auto& pair : disabled_support_contact_pairs_) {
    entries.push_back("policy.disabled_support=" + pair.first + "|" + pair.second);
  }

  std::sort(entries.begin(), entries.end());
  core::ContentHasher h;
  h.mix("manyforge_roboplan/collision_world_revision_fallback");
  h.mix(robot_id);
  for (const auto& entry : entries) {
    h.mix(entry);
  }
  return h.finalize();
}

Eigen::Matrix4d RoboPlanProjectionBackend::pose_to_matrix(
    const core::Pose3DTO& pose) const {
  Eigen::Matrix4d tform = Eigen::Matrix4d::Identity();
  tform(0, 3) = pose.position_m.x_m;
  tform(1, 3) = pose.position_m.y_m;
  tform(2, 3) = pose.position_m.z_m;

  double x = pose.orientation.x;
  double y = pose.orientation.y;
  double z = pose.orientation.z;
  double w = pose.orientation.w;
  tform(0, 0) = 1 - 2*y*y - 2*z*z;
  tform(0, 1) = 2*x*y - 2*z*w;
  tform(0, 2) = 2*x*z + 2*y*w;
  tform(1, 0) = 2*x*y + 2*z*w;
  tform(1, 1) = 1 - 2*x*x - 2*z*z;
  tform(1, 2) = 2*y*z - 2*x*w;
  tform(2, 0) = 2*x*z - 2*y*w;
  tform(2, 1) = 2*y*z + 2*x*w;
  tform(2, 2) = 1 - 2*x*x - 2*y*y;

  return tform;
}

core::Pose3DTO RoboPlanProjectionBackend::matrix_to_pose(
    const Eigen::Matrix4d& tform) const {
  core::Pose3DTO pose;
  pose.position_m.x_m = tform(0, 3);
  pose.position_m.y_m = tform(1, 3);
  pose.position_m.z_m = tform(2, 3);

  Eigen::Matrix3d R = tform.block<3, 3>(0, 0);
  double trace = R.trace();
  double x, y, z, w;

  if (trace > 0) {
    double s = std::sqrt(trace + 1.0) * 2;
    w = 0.25 * s;
    x = (R(2, 1) - R(1, 2)) / s;
    y = (R(0, 2) - R(2, 0)) / s;
    z = (R(1, 0) - R(0, 1)) / s;
  } else if (R(0, 0) > R(1, 1) && R(0, 0) > R(2, 2)) {
    double s = std::sqrt(1.0 + R(0, 0) - R(1, 1) - R(2, 2)) * 2;
    w = (R(2, 1) - R(1, 2)) / s;
    x = 0.25 * s;
    y = (R(0, 1) + R(1, 0)) / s;
    z = (R(0, 2) + R(2, 0)) / s;
  } else if (R(1, 1) > R(2, 2)) {
    double s = std::sqrt(1.0 + R(1, 1) - R(0, 0) - R(2, 2)) * 2;
    w = (R(0, 2) - R(2, 0)) / s;
    x = (R(0, 1) + R(1, 0)) / s;
    y = 0.25 * s;
    z = (R(1, 2) + R(2, 1)) / s;
  } else {
    double s = std::sqrt(1.0 + R(2, 2) - R(0, 0) - R(1, 1)) * 2;
    w = (R(1, 0) - R(0, 1)) / s;
    x = (R(0, 2) + R(2, 0)) / s;
    y = (R(1, 2) + R(2, 1)) / s;
    z = 0.25 * s;
  }

  pose.orientation.x = x;
  pose.orientation.y = y;
  pose.orientation.z = z;
  pose.orientation.w = w;
  return pose;
}

bool RoboPlanProjectionBackend::is_world_frame(const std::string& frame_id) const {
  return frame_id.empty() || frame_id == "universe" || frame_id == "world" ||
         frame_id == "World" || frame_id == "/World";
}

Eigen::Matrix4d RoboPlanProjectionBackend::scene_placement_from_tracked_locked(
    const TrackedObjectState& state) const {
  const Eigen::Matrix4d t_frame_object = pose_to_matrix(state.pose);
  if (is_world_frame(state.frame_id)) {
    return world_from_robot_base_.inverse() * t_frame_object;
  }
  return parent_joint_from_frame_matrix_locked(state.frame_id) * t_frame_object;
}

Eigen::Matrix4d RoboPlanProjectionBackend::parent_joint_from_frame_matrix_locked(
    const std::string& frame_id) const {
  if (is_world_frame(frame_id)) {
    return Eigen::Matrix4d::Identity();
  }

  const auto maybe_frame_id = scene_->getFrameId(frame_id);
  if (!maybe_frame_id.has_value()) {
    throw std::runtime_error(
        "Could not resolve RoboPlan frame '" + frame_id + "': " +
        maybe_frame_id.error());
  }

  const auto& model = scene_->getModel();
  const auto resolved_frame_id = maybe_frame_id.value();
  if (static_cast<size_t>(resolved_frame_id) >= model.frames.size()) {
    throw std::runtime_error(
        "RoboPlan frame id for '" + frame_id + "' is out of range.");
  }

  // Pinocchio GeometryObject::placement is evaluated in the parent joint frame,
  // while ManyForge stores object poses in the named semantic frame.
  return model.frames.at(resolved_frame_id).placement.toHomogeneousMatrix();
}

Eigen::Matrix4d RoboPlanProjectionBackend::frame_to_world_matrix_locked(
    const std::string& frame_id) const {
  if (is_world_frame(frame_id)) {
    return Eigen::Matrix4d::Identity();
  }

  const auto& positions = scene_->getCurrentJointPositions();
  Eigen::VectorXd q(positions.size());
  for (Eigen::Index i = 0; i < positions.size(); ++i) {
    q[i] = positions[i];
  }
  return world_from_robot_base_ * scene_->forwardKinematics(q, frame_id);
}

core::Pose3DTO RoboPlanProjectionBackend::pose_in_base_from_tracked_locked(
    const TrackedObjectState& state, const std::string& base_frame) const {
  const Eigen::Matrix4d t_frame_object = pose_to_matrix(state.pose);
  const Eigen::Matrix4d t_world_frame = frame_to_world_matrix_locked(state.frame_id);
  const Eigen::Matrix4d t_world_object = t_world_frame * t_frame_object;
  const Eigen::Matrix4d t_world_base = frame_to_world_matrix_locked(base_frame);
  return matrix_to_pose(t_world_base.inverse() * t_world_object);
}

bool RoboPlanProjectionBackend::infer_attached_from_frame(
    const std::string& frame_id) const {
  if (is_world_frame(frame_id) || frame_id == "base_link") {
    return false;
  }
  if (scene_) {
    try {
      if (frame_id == robot_model_frame_id_locked()) {
        return false;
      }
    } catch (const std::exception&) {
      // Fall back to the historical conservative rule below.
    }
  }
  return true;
}

std::vector<std::string> RoboPlanProjectionBackend::normalize_touch_links_locked(
    const std::vector<std::string>& touch_links) const {
  std::set<std::string> normalized;
  for (const auto& link_name : touch_links) {
    if (link_name.empty()) {
      continue;
    }
    normalized.insert(link_name);
  }

  return {normalized.begin(), normalized.end()};
}

tl::expected<void, std::string> RoboPlanProjectionBackend::set_touch_links_locked(
    const std::string& object_id,
    const std::vector<std::string>& touch_links,
    bool enable) {
  const auto effective_touch_links = normalize_touch_links_locked(touch_links);
  std::vector<std::string> applied;
  for (const auto& link_name : effective_touch_links) {
    auto result = scene_->setCollisions(object_id, link_name, enable);
    if (!result.has_value()) {
      // Best-effort rollback keeps touch-link updates transactional for the
      // ManyForge collision-world revision contract. If rollback itself fails,
      // report both failures; callers can then publish a policy revision
      // rather than leaving cached validation decisions ambiguous.
      std::string rollback_error;
      for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
        auto rollback = scene_->setCollisions(object_id, *it, !enable);
        if (!rollback.has_value()) {
          if (!rollback_error.empty()) {
            rollback_error += "; ";
          }
          rollback_error += rollback.error();
        }
      }
      if (!rollback_error.empty()) {
        return tl::make_unexpected(
            result.error() + " (rollback failed: " + rollback_error + ")");
      }
      return tl::make_unexpected(result.error());
    }
    applied.push_back(link_name);
  }
  return {};
}

tl::expected<void, std::string> RoboPlanProjectionBackend::clear_touch_links_locked(
    const std::string& object_id,
    TrackedObjectState& tracked) {
  if (tracked.touch_links.empty()) {
    return {};
  }
  auto result = set_touch_links_locked(object_id, tracked.touch_links, true);
  if (!result.has_value()) {
    return result;
  }
  tracked.touch_links.clear();
  return {};
}

// Temporary RoboPlan adapter policy:
// Scene::hasCollisions(q) currently validates the whole Pinocchio geometry
// model, so detached scene object <-> detached scene object contacts can reject
// an otherwise valid robot start/goal state. ManyForge treats those contacts as
// scene-quality data, not motion blockers. Until RoboPlan exposes a robot-state
// collision query that filters static-static pairs, keep detached object pairs
// disabled in the backend projection and re-enable a pair as soon as either
// object is attached to a robot link.
std::pair<std::string, std::string> RoboPlanProjectionBackend::canonical_collision_pair(
    const std::string& body1,
    const std::string& body2) const {
  if (body1 < body2) {
    return {body1, body2};
  }
  return {body2, body1};
}

std::string RoboPlanProjectionBackend::collision_pair_text(
    const std::pair<std::string, std::string>& pair) const {
  return pair.first + " <-> " + pair.second;
}

std::optional<std::pair<std::string, std::string>>
RoboPlanProjectionBackend::canonical_object_collision_pair_locked(
    const std::string& body1,
    const std::string& body2,
    const RoboPlanCollisionBodyIdMapper& diagnostic_mapper) const {
  const auto object_id_for_body =
      [&](const std::string& raw_body) -> std::optional<std::string> {
    if (object_states_.find(raw_body) != object_states_.end()) {
      return raw_body;
    }
    if (auto normalized = object_id_from_normalized_body_id(raw_body)) {
      if (object_states_.find(*normalized) != object_states_.end()) {
        return normalized;
      }
    }
    auto mapped = diagnostic_mapper.lookup(raw_body);
    if (!mapped.has_value()) {
      return std::nullopt;
    }
    auto object_id = object_id_from_normalized_body_id(*mapped);
    if (object_id.has_value() &&
        object_states_.find(*object_id) != object_states_.end()) {
      return object_id;
    }
    return std::nullopt;
  };

  auto object_a = object_id_for_body(body1);
  auto object_b = object_id_for_body(body2);
  if (!object_a.has_value() || !object_b.has_value()) {
    return std::nullopt;
  }
  return canonical_collision_pair(*object_a, *object_b);
}

RoboPlanProjectionBackend::CollisionDiagnostics
RoboPlanProjectionBackend::collect_collision_diagnostics_locked(
    const Eigen::VectorXd& q) {
  CollisionDiagnostics diagnostics;

  diagnostics.raw_output = capture_collision_diagnostics_stdout([&]() {
    scene_->hasCollisions(q, true);
  });

  std::string robot_id;
  {
    std::scoped_lock map_lock(mapper_mutex_);
    robot_id = mapper_robot_id_;
  }
  RoboPlanCollisionBodyIdMapper diagnostic_mapper;
  diagnostic_mapper.rebuild(
      robot_id,
      scene_objects_locked(),
      robot_body_mappings_locked(robot_id));

  std::istringstream lines(diagnostics.raw_output);
  std::string line;
  const std::string prefix = "Collision detected between ";
  while (std::getline(lines, line)) {
    if (line.rfind(prefix, 0) != 0) {
      continue;
    }
    const std::string pair_text = line.substr(prefix.size());
    const std::string needle = " and ";
    const auto split = pair_text.find(needle);
    if (split == std::string::npos) {
      continue;
    }
    CollisionPairDiagnostic pair;
    pair.body_a = pair_text.substr(0, split);
    pair.body_b = pair_text.substr(split + needle.size());
    pair.canonical_pair = canonical_collision_pair(pair.body_a, pair.body_b);
    pair.canonical_object_pair =
        canonical_object_collision_pair_locked(
            pair.body_a, pair.body_b, diagnostic_mapper);
    pair.text = collision_pair_text(pair.canonical_pair);
    diagnostics.pairs.push_back(std::move(pair));
  }

  return diagnostics;
}

void RoboPlanProjectionBackend::add_collision_details(
    std::map<std::string, std::string>& details,
    const CollisionDiagnostics& diagnostics) const {
  details["collision_pair_count"] = std::to_string(diagnostics.pairs.size());
  std::string joined;
  for (size_t i = 0; i < diagnostics.pairs.size(); ++i) {
    if (!joined.empty()) {
      joined += "; ";
    }
    joined += diagnostics.pairs[i].text;
    details["collision_pair_" + std::to_string(i + 1)] =
        diagnostics.pairs[i].text;
    if (diagnostics.pairs[i].canonical_object_pair.has_value()) {
      details["collision_object_pair_" + std::to_string(i + 1)] =
          collision_pair_text(*diagnostics.pairs[i].canonical_object_pair);
    }
  }
  if (!joined.empty()) {
    details["collision_pairs"] = joined;
    details["contact_body_a"] = diagnostics.pairs.front().canonical_pair.first;
    details["contact_body_b"] = diagnostics.pairs.front().canonical_pair.second;
  }
}

tl::expected<void, std::string>
RoboPlanProjectionBackend::apply_collision_pair_projection_locked(
    const std::pair<std::string, std::string>& pair) {
  const bool should_disable =
      disabled_static_collision_pairs_.find(pair) != disabled_static_collision_pairs_.end() ||
      disabled_support_contact_pairs_.find(pair) != disabled_support_contact_pairs_.end();
  auto result = scene_->setCollisions(pair.first, pair.second, !should_disable);
  if (!result.has_value()) {
    return tl::make_unexpected(result.error());
  }
  return {};
}

tl::expected<void, std::string>
RoboPlanProjectionBackend::apply_static_collision_policy_for_object_locked(
    const std::string& object_id) {
  const auto target_it = object_states_.find(object_id);
  if (target_it == object_states_.end()) {
    return {};
  }

  for (const auto& [other_id, other_state] : object_states_) {
    if (other_id == object_id) {
      continue;
    }

    const auto pair = canonical_collision_pair(object_id, other_id);
    const bool should_disable =
        !target_it->second.attached && !other_state.attached;
    const bool is_disabled =
        disabled_static_collision_pairs_.find(pair) !=
        disabled_static_collision_pairs_.end();

    if (should_disable == is_disabled) {
      continue;
    }

    if (should_disable) {
      disabled_static_collision_pairs_.insert(pair);
    } else {
      disabled_static_collision_pairs_.erase(pair);
    }
    auto result = apply_collision_pair_projection_locked(pair);
    if (!result.has_value()) {
      return result;
    }
  }

  return {};
}

void RoboPlanProjectionBackend::erase_static_collision_policy_for_object_locked(
    const std::string& object_id) {
  for (auto it = disabled_static_collision_pairs_.begin();
       it != disabled_static_collision_pairs_.end();) {
    if (it->first == object_id || it->second == object_id) {
      const auto pair = *it;
      it = disabled_static_collision_pairs_.erase(it);
      (void)apply_collision_pair_projection_locked(pair);
    } else {
      ++it;
    }
  }
}

tl::expected<void, std::string>
RoboPlanProjectionBackend::recompute_support_contact_policy_locked() {
  std::set<std::pair<std::string, std::string>> candidate_pairs;
  std::set<std::pair<std::string, std::string>> explicit_pairs;

  // Automatic support-contact discovery:
  // an attached object is part of the moving system, so only its current
  // contacts with detached world objects are allowed to seed a support release
  // phase. Other attached-object/world-object pairs remain enabled and will
  // block planning/validation as normal obstacles.
  for (const auto& [object_id, tracked] : object_states_) {
    if (!tracked.attached) {
      continue;
    }
    for (const auto& [other_id, other_state] : object_states_) {
      if (other_id == object_id || other_state.attached) {
        continue;
      }
      candidate_pairs.insert(canonical_collision_pair(object_id, other_id));
    }
  }

  // Explicit scene metadata is kept as an override for hand-authored programs
  // while RoboPlan lacks a first-class scoped-contact query. It is no longer the
  // primary authoring mechanism: current attached-object contacts are discovered
  // dynamically above.
  std::set<std::pair<std::string, std::string>> desired_pairs;
  for (const auto& [object_id, tracked] : object_states_) {
    if (!tracked.attached) {
      continue;
    }
    for (const auto& support_contact : tracked.support_contacts) {
      if (support_contact.support_object_id.empty()) {
        continue;
      }
      const auto support_it = object_states_.find(support_contact.support_object_id);
      if (support_it == object_states_.end() || support_it->second.attached) {
        continue;
      }
      explicit_pairs.insert(
          canonical_collision_pair(object_id, support_contact.support_object_id));
    }
  }

  for (const auto& pair : candidate_pairs) {
    auto result = scene_->setCollisions(pair.first, pair.second, true);
    if (!result.has_value()) {
      return tl::make_unexpected(result.error());
    }
  }

  if (!candidate_pairs.empty()) {
    const auto q = scene_->getCurrentJointPositions();
    const auto diagnostics = collect_collision_diagnostics_locked(q);
    for (const auto& diagnostic : diagnostics.pairs) {
      const auto support_pair =
          diagnostic.canonical_object_pair.value_or(diagnostic.canonical_pair);
      if (candidate_pairs.find(support_pair) != candidate_pairs.end()) {
        desired_pairs.insert(support_pair);
      }
    }
  }
  desired_pairs.insert(explicit_pairs.begin(), explicit_pairs.end());

  std::set<std::pair<std::string, std::string>> touched_pairs =
      disabled_support_contact_pairs_;
  touched_pairs.insert(candidate_pairs.begin(), candidate_pairs.end());
  touched_pairs.insert(desired_pairs.begin(), desired_pairs.end());

  disabled_support_contact_pairs_ = std::move(desired_pairs);
  for (const auto& pair : touched_pairs) {
    if (object_states_.find(pair.first) == object_states_.end() ||
        object_states_.find(pair.second) == object_states_.end()) {
      continue;
    }
    auto result = apply_collision_pair_projection_locked(pair);
    if (!result.has_value()) {
      return result;
    }
  }
  return {};
}

tl::expected<void, std::string>
RoboPlanProjectionBackend::set_support_contact_pairs_for_validation_locked(
    bool enable) {
  const auto pairs = enable
      ? active_support_contact_pairs_for_validation_locked()
      : disabled_support_contact_pairs_;
  for (const auto& pair : pairs) {
    auto result = scene_->setCollisions(pair.first, pair.second, enable);
    if (!result.has_value()) {
      return tl::make_unexpected(result.error());
    }
  }
  return {};
}

std::set<std::pair<std::string, std::string>>
RoboPlanProjectionBackend::active_support_contact_pairs_for_validation_locked() const {
  return disabled_support_contact_pairs_;
}

bool RoboPlanProjectionBackend::is_active_support_contact_pair_locked(
    const std::pair<std::string, std::string>& pair) const {
  return is_support_contact_pair_locked(pair);
}

bool RoboPlanProjectionBackend::is_support_contact_pair_locked(
    const std::pair<std::string, std::string>& pair) const {
  return disabled_support_contact_pairs_.find(pair) !=
         disabled_support_contact_pairs_.end();
}

std::optional<core::SupportContactDTO>
RoboPlanProjectionBackend::support_contact_policy_for_pair_locked(
    const std::pair<std::string, std::string>& pair) const {
  for (const auto& [object_id, tracked] : object_states_) {
    for (const auto& support_contact : tracked.support_contacts) {
      if (support_contact.support_object_id.empty()) {
        continue;
      }
      if (canonical_collision_pair(object_id, support_contact.support_object_id) == pair) {
        return support_contact;
      }
    }
  }
  return std::nullopt;
}

void RoboPlanProjectionBackend::record_scene_event_locked(SceneTraceEvent event) {
  if (event.timestamp_ns == 0) {
    event.timestamp_ns = steady_now_ns();
  }
  scene_trace_.push_back(std::move(event));
  constexpr size_t kMaxSceneTraceEvents = 4096;
  while (scene_trace_.size() > kMaxSceneTraceEvents) {
    scene_trace_.pop_front();
  }
}

tl::expected<void, std::string> RoboPlanProjectionBackend::add_geometry_locked(
    const std::string& object_id, const TrackedObjectState& state) {
  const std::string target_frame =
      is_world_frame(state.frame_id) ? "universe" : state.frame_id;
  Eigen::Matrix4d t_target_object = scene_placement_from_tracked_locked(state);

  const Eigen::Vector4d color(1.0, 0.0, 0.0, 1.0);
  if (auto* box = std::get_if<core::BoxShapeDTO>(&state.shape)) {
    roboplan::Box b(box->dimensions_m.x_m, box->dimensions_m.y_m, box->dimensions_m.z_m);
    auto r = scene_->addBoxGeometry(object_id, target_frame, b, t_target_object, color);
    if (!r.has_value()) {
      return tl::make_unexpected(r.error());
    }
  } else if (auto* sphere = std::get_if<core::SphereShapeDTO>(&state.shape)) {
    roboplan::Sphere s(sphere->radius_m);
    auto r = scene_->addSphereGeometry(object_id, target_frame, s, t_target_object, color);
    if (!r.has_value()) {
      return tl::make_unexpected(r.error());
    }
  } else if (auto* cylinder = std::get_if<core::CylinderShapeDTO>(&state.shape)) {
    roboplan::Cylinder c(cylinder->radius_m, cylinder->height_m);
    auto r = scene_->addCylinderGeometry(object_id, target_frame, c, t_target_object, color);
    if (!r.has_value()) {
      return tl::make_unexpected(r.error());
    }
  } else if (auto* mesh = std::get_if<core::MeshShapeDTO>(&state.shape)) {
    auto mesh_path = resolve_mesh_resource_uri(mesh->resource_uri);
    if (!mesh_path.has_value()) {
      return tl::make_unexpected(mesh_path.error());
    }
    const Eigen::Vector3d mesh_scale(
        mesh->scale.x_m == 0.0 ? 1.0 : mesh->scale.x_m,
        mesh->scale.y_m == 0.0 ? 1.0 : mesh->scale.y_m,
        mesh->scale.z_m == 0.0 ? 1.0 : mesh->scale.z_m);
    roboplan::Mesh mesh_geometry(*mesh_path, mesh_scale);
    auto r = scene_->addMeshGeometry(
        object_id, target_frame, mesh_geometry, t_target_object, color);
    if (!r.has_value()) {
      return tl::make_unexpected(r.error());
    }
  } else {
    return tl::make_unexpected(
        "add_geometry_locked: unsupported shape type for object '" + object_id + "'");
  }
  return {};
}

core::SceneEditResultDTO RoboPlanProjectionBackend::reparent_object_locked(
    const std::string& object_id, const std::string& target_frame_id, bool emit_event) {
  core::SceneEditResultDTO result;
  const std::string canonical_target_frame =
      is_world_frame(target_frame_id) ? "universe" : target_frame_id;
  const auto it = object_states_.find(object_id);
  if (it == object_states_.end()) {
    result.error = core::ErrorDTO{
        core::kErrorObjectNotFound, "Object not found: " + object_id, {}};
    SceneTraceEvent event;
    event.event = is_world_frame(canonical_target_frame) ? "detach" : "attach";
    event.object_id = object_id;
    event.success = false;
    event.frame_id = canonical_target_frame;
    event.error = result.error->message;
    if (emit_event) {
      record_scene_event_locked(std::move(event));
    }
    return result;
  }

  // Compute target-frame pose from current world pose.
  const Eigen::Matrix4d t_frame_object = pose_to_matrix(it->second.pose);
  const Eigen::Matrix4d t_world_frame = frame_to_world_matrix_locked(it->second.frame_id);
  const Eigen::Matrix4d t_world_object = t_world_frame * t_frame_object;
  const Eigen::Matrix4d t_world_target = frame_to_world_matrix_locked(canonical_target_frame);
  const Eigen::Matrix4d t_target_object = t_world_target.inverse() * t_world_object;

  TrackedObjectState new_state = it->second;
  new_state.pose = matrix_to_pose(t_target_object);
  new_state.frame_id = canonical_target_frame;
  new_state.attached = infer_attached_from_frame(canonical_target_frame);
  // Reparenting changes pose authority: the object is now owned by the robot
  // attachment chain or by an authored world-frame detach until a fresh
  // pose-source update explicitly reclaims authority.
  new_state.source_timestamp_ns.reset();
  new_state.source_freshness_budget_ns.reset();

  Eigen::Matrix4d t_scene_object = scene_placement_from_tracked_locked(new_state);
  const std::string scene_target_frame =
      is_world_frame(new_state.frame_id) ? "universe" : new_state.frame_id;
  auto r = scene_->updateGeometryPlacement(object_id, scene_target_frame, t_scene_object);
  if (!r.has_value()) {
    result.error = core::ErrorDTO{core::kErrorInternalError, r.error(), {}};
    SceneTraceEvent event;
    event.event = is_world_frame(canonical_target_frame) ? "detach" : "attach";
    event.object_id = object_id;
    event.success = false;
    event.frame_id = canonical_target_frame;
    event.attached = it->second.attached;
    event.pose_in_frame = it->second.pose;
    event.pose_in_universe = pose_in_base_from_tracked_locked(it->second, "universe");
    event.error = result.error->message;
    if (emit_event) {
      record_scene_event_locked(std::move(event));
    }
    return result;
  }
  object_states_[object_id] = new_state;
  auto policy_result = apply_static_collision_policy_for_object_locked(object_id);
  if (!policy_result.has_value()) {
    result.error = core::ErrorDTO{
        core::kErrorInternalError,
        "Failed to apply static object collision policy: " + policy_result.error(),
        {},
    };
    SceneTraceEvent event;
    event.event = is_world_frame(canonical_target_frame) ? "detach" : "attach";
    event.object_id = object_id;
    event.success = false;
    event.frame_id = canonical_target_frame;
    event.attached = it->second.attached;
    event.pose_in_frame = it->second.pose;
    event.pose_in_universe = pose_in_base_from_tracked_locked(it->second, "universe");
    event.error = result.error->message;
    if (emit_event) {
      record_scene_event_locked(std::move(event));
    }
    return result;
  }
  auto support_policy_result = recompute_support_contact_policy_locked();
  if (!support_policy_result.has_value()) {
    result.error = core::ErrorDTO{
        core::kErrorInternalError,
        "Failed to apply support-contact collision policy: " +
            support_policy_result.error(),
        {},
    };
    SceneTraceEvent event;
    event.event = is_world_frame(canonical_target_frame) ? "detach" : "attach";
    event.object_id = object_id;
    event.success = false;
    event.frame_id = canonical_target_frame;
    event.attached = it->second.attached;
    event.pose_in_frame = it->second.pose;
    event.pose_in_universe = pose_in_base_from_tracked_locked(it->second, "universe");
    event.error = result.error->message;
    if (emit_event) {
      record_scene_event_locked(std::move(event));
    }
    return result;
  }

  const auto& tracked = object_states_.at(object_id);
  result.success = true;
  SceneTraceEvent event;
  event.event = tracked.attached ? "attach" : "detach";
  event.object_id = object_id;
  event.success = true;
  event.frame_id = tracked.frame_id;
  event.attached = tracked.attached;
  event.pose_in_frame = tracked.pose;
  event.pose_in_universe = pose_in_base_from_tracked_locked(tracked, "universe");
  if (emit_event) {
    record_scene_event_locked(std::move(event));
  }
  return result;
}

core::SceneEditResultDTO RoboPlanProjectionBackend::add_object(
    const core::AddObjectRequestDTO& request) {
  core::SceneEditResultDTO result;

  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);

    const std::string final_frame_id =
        is_world_frame(request.frame_id) ? "universe" : request.frame_id;
    const bool attached = infer_attached_from_frame(final_frame_id);

    TrackedObjectState new_state;
    new_state.pose = request.pose;
    new_state.shape = request.shape;
    new_state.frame_id = final_frame_id;
    new_state.attached = attached;
    new_state.support_contacts = request.support_contacts;

    auto add_r = add_geometry_locked(request.object_id, new_state);
    if (!add_r.has_value()) {
      result.error = core::ErrorDTO{core::kErrorInternalError, add_r.error(), {}};
      return result;
    }
    object_states_[request.object_id] = new_state;
    auto policy_result =
        apply_static_collision_policy_for_object_locked(request.object_id);
    if (!policy_result.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInternalError,
          "Failed to apply static object collision policy: " + policy_result.error(),
          {},
      };
      return result;
    }
    auto support_policy_result = recompute_support_contact_policy_locked();
    if (!support_policy_result.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInternalError,
          "Failed to apply support-contact collision policy: " +
              support_policy_result.error(),
          {},
      };
      return result;
    }

    const auto& tracked = object_states_.at(request.object_id);
    result.success = true;
    publish_collision_world_revision_locked(
        core::CollisionWorldMutationSource::kSceneEdit,
        object_body_ids_for_state_locked(request.object_id, tracked),
        "scene_edit.add_object");
    SceneTraceEvent event;
    event.event = "add";
    event.object_id = request.object_id;
    event.success = true;
    event.frame_id = tracked.frame_id;
    event.attached = tracked.attached;
    event.pose_in_frame = tracked.pose;
    event.pose_in_universe = pose_in_base_from_tracked_locked(tracked, "universe");
    record_scene_event_locked(std::move(event));
  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorInternalError, e.what(), {}};
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    SceneTraceEvent event;
    event.event = "add";
    event.object_id = request.object_id;
    event.success = false;
    event.frame_id = request.frame_id;
    event.error = e.what();
    record_scene_event_locked(std::move(event));
  }

  return result;
}

core::SceneEditResultDTO RoboPlanProjectionBackend::remove_object(
    const core::RemoveObjectRequestDTO& request) {
  core::SceneEditResultDTO result;

  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    const auto it = object_states_.find(request.object_id);
    if (it == object_states_.end()) {
      result.error = core::ErrorDTO{
          core::kErrorObjectNotFound,
          "Could not find object '" + request.object_id + "' to remove.",
          {}};
      SceneTraceEvent event;
      event.event = "remove";
      event.object_id = request.object_id;
      event.success = false;
      event.error = result.error->message;
      record_scene_event_locked(std::move(event));
      result.rollback_proven = true;
      return result;
    }
    const TrackedObjectState tracked_before = it->second;

    auto r = scene_->removeGeometry(request.object_id);
    if (!r.has_value()) {
      result.error = core::ErrorDTO{core::kErrorInternalError, r.error(), {}};
      SceneTraceEvent event;
      event.event = "remove";
      event.object_id = request.object_id;
      event.success = false;
      event.error = result.error->message;
      record_scene_event_locked(std::move(event));
      return result;
    }
    erase_static_collision_policy_for_object_locked(request.object_id);
    object_states_.erase(request.object_id);
    auto support_policy_result = recompute_support_contact_policy_locked();
    if (!support_policy_result.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInternalError,
          "Failed to apply support-contact collision policy after remove: " +
              support_policy_result.error(),
          {},
      };
      SceneTraceEvent event;
      event.event = "remove";
      event.object_id = request.object_id;
      event.success = false;
      event.error = result.error->message;
      record_scene_event_locked(std::move(event));
      return result;
    }

    SceneTraceEvent event;
    event.event = "remove";
    event.object_id = request.object_id;
    event.success = true;
    event.frame_id = tracked_before.frame_id;
    event.attached = tracked_before.attached;
    event.pose_in_frame = tracked_before.pose;
    event.pose_in_universe =
        pose_in_base_from_tracked_locked(tracked_before, "universe");
    result.success = true;
    publish_collision_world_revision_locked(
        core::CollisionWorldMutationSource::kSceneEdit,
        object_body_ids_for_state_locked(request.object_id, tracked_before),
        "scene_edit.remove_object");
    record_scene_event_locked(std::move(event));
  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorInternalError, e.what(), {}};
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    SceneTraceEvent event;
    event.event = "remove";
    event.object_id = request.object_id;
    event.success = false;
    event.error = e.what();
    record_scene_event_locked(std::move(event));
  }

  return result;
}

core::SceneEditResultDTO RoboPlanProjectionBackend::update_object_pose(
    const core::UpdateObjectPoseRequestDTO& request) {
  core::SceneEditResultDTO result;
  result.rollback_proven = true;

  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    const auto it = object_states_.find(request.object_id);
    if (it == object_states_.end()) {
      result.error = core::ErrorDTO{
          core::kErrorObjectNotFound,
          "Could not find object '" + request.object_id + "' to update.",
          {}};
      return result;
    }

    const TrackedObjectState previous_state = it->second;
    const auto previous_static_pairs = disabled_static_collision_pairs_;
    const auto previous_support_pairs = disabled_support_contact_pairs_;
    TrackedObjectState new_state = it->second;
    new_state.pose = request.pose;
    new_state.frame_id = is_world_frame(request.frame_id) ? "universe" : request.frame_id;
    new_state.attached = infer_attached_from_frame(new_state.frame_id);
    if (request.source_timestamp_ns.has_value() !=
        request.source_freshness_budget_ns.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInvalidRequest,
          "source_timestamp_ns and source_freshness_budget_ns must be provided "
          "together for source-driven pose updates",
          {},
      };
      return result;
    }
    if (request.source_timestamp_ns && *request.source_timestamp_ns < 0) {
      result.error = core::ErrorDTO{core::kErrorInvalidRequest,
                                    "source_timestamp_ns must be non-negative",
                                    {}};
      return result;
    }
    if (request.source_freshness_budget_ns &&
        *request.source_freshness_budget_ns <= 0) {
      result.error = core::ErrorDTO{
          core::kErrorInvalidRequest,
          "source_freshness_budget_ns must be positive",
          {},
      };
      return result;
    }
    if (optional_text_blank(request.source_id)) {
      result.error = core::ErrorDTO{core::kErrorInvalidRequest,
                                    "source_id must be non-empty when provided",
                                    {}};
      return result;
    }
    const bool scene_admission_present =
        request.scene_admission_role.has_value() ||
        request.scene_admission_lease_max_age_ns.has_value() ||
        request.scene_admission_target_drift_tolerance_m.has_value() ||
        request.scene_admission_target_angular_tolerance_rad.has_value() ||
        request.scene_admission_obstacle_drift_tolerance_m.has_value() ||
        request.scene_admission_obstacle_angular_tolerance_rad.has_value() ||
        request.scene_admission_allow_runtime_dynamic_obstacle;
    if (scene_admission_present &&
        !request.scene_admission_role.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInvalidRequest,
          "scene_admission_role is required when scene admission metadata is "
          "provided",
          {}};
      return result;
    }
    auto parsed_scene_admission_role =
        observed_scene_role_from_string(request.scene_admission_role);
    if (request.scene_admission_role.has_value() &&
        !parsed_scene_admission_role.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInvalidRequest,
          "scene_admission_role must be one of static_obstacle, "
          "snapshot_obstacle, observed_target, observed_dynamic_obstacle, "
          "attached_object, support_contact, visualization_only, "
          "robot_runtime_state",
          {}};
      return result;
    }
    if (request.scene_admission_lease_max_age_ns.has_value() &&
        *request.scene_admission_lease_max_age_ns <= 0) {
      result.error =
          core::ErrorDTO{core::kErrorInvalidRequest,
                         "scene_admission_lease_max_age_ns must be positive",
                         {}};
      return result;
    }
    if (!non_negative_optional_double(
            request.scene_admission_target_drift_tolerance_m) ||
        !non_negative_optional_double(
            request.scene_admission_target_angular_tolerance_rad) ||
        !non_negative_optional_double(
            request.scene_admission_obstacle_drift_tolerance_m) ||
        !non_negative_optional_double(
            request.scene_admission_obstacle_angular_tolerance_rad)) {
      result.error = core::ErrorDTO{
          core::kErrorInvalidRequest,
          "scene_admission drift tolerances must be finite and >= 0",
          {}};
      return result;
    }
    if (parsed_scene_admission_role !=
            core::ObservedSceneObjectRole::kObservedTarget &&
        (request.scene_admission_target_drift_tolerance_m.has_value() ||
         request.scene_admission_target_angular_tolerance_rad.has_value())) {
      result.error = core::ErrorDTO{
          core::kErrorInvalidRequest,
          "scene_admission target drift tolerances are only valid for "
          "observed_target",
          {}};
      return result;
    }
    if (parsed_scene_admission_role !=
            core::ObservedSceneObjectRole::kObservedDynamicObstacle &&
        (request.scene_admission_obstacle_drift_tolerance_m.has_value() ||
         request.scene_admission_obstacle_angular_tolerance_rad.has_value())) {
      result.error = core::ErrorDTO{
          core::kErrorInvalidRequest,
          "scene_admission obstacle drift tolerances are only valid for "
          "observed_dynamic_obstacle",
          {}};
      return result;
    }
    if (parsed_scene_admission_role !=
            core::ObservedSceneObjectRole::kObservedDynamicObstacle &&
        request.scene_admission_allow_runtime_dynamic_obstacle) {
      result.error = core::ErrorDTO{
          core::kErrorInvalidRequest,
          "scene_admission_allow_runtime_dynamic_obstacle is only valid for "
          "observed_dynamic_obstacle",
          {}};
      return result;
    }
    if (parsed_scene_admission_role ==
            core::ObservedSceneObjectRole::kRobotRuntimeState ||
        parsed_scene_admission_role ==
            core::ObservedSceneObjectRole::kVisualizationOnly) {
      result.error = core::ErrorDTO{
          core::kErrorInvalidRequest,
          "scene_admission_role for a promoted collision-scene object must not "
          "be robot_runtime_state or visualization_only",
          {}};
      return result;
    }
    new_state.source_timestamp_ns = request.source_timestamp_ns;
    new_state.source_freshness_budget_ns = request.source_freshness_budget_ns;
    new_state.source_id = request.source_id;
    new_state.scene_admission_role = parsed_scene_admission_role;
    new_state.scene_admission_lease_max_age_ns =
        request.scene_admission_lease_max_age_ns;
    new_state.scene_admission_target_drift_tolerance_m =
        request.scene_admission_target_drift_tolerance_m;
    new_state.scene_admission_target_angular_tolerance_rad =
        request.scene_admission_target_angular_tolerance_rad;
    new_state.scene_admission_obstacle_drift_tolerance_m =
        request.scene_admission_obstacle_drift_tolerance_m;
    new_state.scene_admission_obstacle_angular_tolerance_rad =
        request.scene_admission_obstacle_angular_tolerance_rad;
    new_state.scene_admission_allow_runtime_dynamic_obstacle =
        request.scene_admission_allow_runtime_dynamic_obstacle;

    std::string robot_id;
    {
      std::scoped_lock map_lock(mapper_mutex_);
      robot_id = mapper_robot_id_;
    }
    const auto classification = classify_pose_update_locked(
        request.object_id, previous_state, new_state, robot_id);
    if (classification == PoseUpdateClassification::kNoOp) {
      ++scene_update_no_op_count_;
      result.success = true;
      result.update_status = "no_op";
      return result;
    }
    if (classification == PoseUpdateClassification::kFreshnessOnly) {
      result.rollback_proven = false;
      object_states_[request.object_id] = new_state;
      publish_runtime_body_freshness_locked();
      ++scene_update_freshness_only_count_;
      result.success = true;
      result.update_status = "freshness_only";
      result.collision_world_freshness_changed = true;
      return result;
    }

    result.rollback_proven = false;
    Eigen::Matrix4d tform = scene_placement_from_tracked_locked(new_state);
    const std::string scene_target_frame =
        is_world_frame(new_state.frame_id) ? "universe" : new_state.frame_id;
    auto r = scene_->updateGeometryPlacement(request.object_id, scene_target_frame, tform);
    if (!r.has_value()) {
      result.error = core::ErrorDTO{core::kErrorInternalError, r.error(), {}};
      return result;
    }
    object_states_[request.object_id] = new_state;
    auto policy_result =
        apply_static_collision_policy_for_object_locked(request.object_id);
    if (!policy_result.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInternalError,
          "Failed to apply static object collision policy: " + policy_result.error(),
          {},
      };
      return result;
    }
    auto support_policy_result = recompute_support_contact_policy_locked();
    if (!support_policy_result.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInternalError,
          "Failed to apply support-contact collision policy: " +
              support_policy_result.error(),
          {},
      };
      return result;
    }

    const auto& tracked = object_states_.at(request.object_id);
    result.success = true;
    const bool pose_source_only =
        previous_state.frame_id == tracked.frame_id &&
        previous_state.attached == tracked.attached;
    const bool policy_changed =
        previous_static_pairs != disabled_static_collision_pairs_ ||
        previous_support_pairs != disabled_support_contact_pairs_;
    publish_collision_world_revision_locked(
        pose_source_only
            ? (policy_changed
                   ? core::CollisionWorldMutationSource::kPolicyUpdate
                   : core::CollisionWorldMutationSource::kPoseSourceApply)
            : core::CollisionWorldMutationSource::kSceneEdit,
        object_body_ids_for_transition_locked(
            request.object_id, previous_state, tracked),
        "scene_edit.update_object_pose");
    result.update_status = "geometry_changed";
    result.collision_world_revision_changed = true;
    result.collision_world_freshness_changed =
        previous_state.source_timestamp_ns != tracked.source_timestamp_ns ||
        previous_state.source_freshness_budget_ns !=
            tracked.source_freshness_budget_ns ||
        previous_state.source_id != tracked.source_id;
    ++scene_update_geometry_changed_count_;
    SceneTraceEvent event;
    event.event = "update_pose";
    event.object_id = request.object_id;
    event.success = true;
    event.frame_id = tracked.frame_id;
    event.attached = tracked.attached;
    event.pose_in_frame = tracked.pose;
    event.pose_in_universe = pose_in_base_from_tracked_locked(tracked, "universe");
    record_scene_event_locked(std::move(event));
  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorInternalError, e.what(), {}};
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    SceneTraceEvent event;
    event.event = "update_pose";
    event.object_id = request.object_id;
    event.success = false;
    event.frame_id = request.frame_id;
    event.error = e.what();
    record_scene_event_locked(std::move(event));
  }

  return result;
}

core::SceneEditResultDTO RoboPlanProjectionBackend::attach_object(
    const core::AttachObjectRequestDTO& request) {
  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    if (request.frame_id.empty()) {
      auto result = core::SceneEditResultDTO{
          false,
          core::ErrorDTO{core::kErrorInvalidRequest,
                         "Attach target frame_id must not be empty", {}}};
      SceneTraceEvent event;
      event.event = "attach";
      event.object_id = request.object_id;
      event.success = false;
      event.error = result.error->message;
      record_scene_event_locked(std::move(event));
      result.rollback_proven = true;
      return result;
    }
    const auto it = object_states_.find(request.object_id);
    if (it == object_states_.end()) {
      auto result = core::SceneEditResultDTO{
          false,
          core::ErrorDTO{core::kErrorObjectNotFound,
                         "Object not found: " + request.object_id, {}}};
      SceneTraceEvent event;
      event.event = "attach";
      event.object_id = request.object_id;
      event.success = false;
      event.frame_id = request.frame_id;
      event.error = result.error->message;
      record_scene_event_locked(std::move(event));
      result.rollback_proven = true;
      return result;
    }
    auto& tracked = it->second;
    const TrackedObjectState state_before = tracked;
    auto clear_result = clear_touch_links_locked(request.object_id, tracked);
    if (!clear_result.has_value()) {
      if (!state_before.touch_links.empty()) {
        publish_collision_world_revision_locked(
            core::CollisionWorldMutationSource::kPolicyUpdate,
            object_body_ids_for_state_locked(request.object_id, tracked),
            "scene_edit.attach_object.partial_failure");
      }
      return core::SceneEditResultDTO{
          false,
          core::ErrorDTO{
              core::kErrorInternalError,
              "Failed to clear existing touch links before attach: " + clear_result.error(),
              {},
          },
      };
    }
    auto result = reparent_object_locked(request.object_id, request.frame_id);
    if (!result.success) {
      if (state_before.touch_links != tracked.touch_links) {
        publish_collision_world_revision_locked(
            core::CollisionWorldMutationSource::kPolicyUpdate,
            object_body_ids_for_state_locked(request.object_id, tracked),
            "scene_edit.attach_object.partial_failure");
      }
      return result;
    }
    auto& attached_state = object_states_.at(request.object_id);
    attached_state.source_id.reset();
    attached_state.source_timestamp_ns.reset();
    attached_state.source_freshness_budget_ns.reset();
    attached_state.scene_admission_role.reset();
    attached_state.scene_admission_lease_max_age_ns.reset();
    attached_state.scene_admission_target_drift_tolerance_m.reset();
    attached_state.scene_admission_target_angular_tolerance_rad.reset();
    attached_state.scene_admission_obstacle_drift_tolerance_m.reset();
    attached_state.scene_admission_obstacle_angular_tolerance_rad.reset();
    attached_state.scene_admission_allow_runtime_dynamic_obstacle = false;
    if (!request.touch_links.empty()) {
      const auto effective_touch_links =
          normalize_touch_links_locked(request.touch_links);
      auto collision_result =
          set_touch_links_locked(request.object_id, effective_touch_links, false);
      if (!collision_result.has_value()) {
        publish_collision_world_revision_locked(
            core::CollisionWorldMutationSource::kAttach,
            object_body_ids_for_transition_locked(
                request.object_id, state_before, object_states_.at(request.object_id)),
            "scene_edit.attach_object.partial_failure");
        return core::SceneEditResultDTO{
            false,
            core::ErrorDTO{
                core::kErrorInternalError,
                "Failed to set touch links for attached object: " + collision_result.error(),
                {},
            },
        };
      }
      tracked.touch_links = effective_touch_links;
    }
    publish_collision_world_revision_locked(
        core::CollisionWorldMutationSource::kAttach,
        object_body_ids_for_transition_locked(
            request.object_id, state_before, object_states_.at(request.object_id)),
        "scene_edit.attach_object");
    return result;
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    SceneTraceEvent event;
    event.event = "attach";
    event.object_id = request.object_id;
    event.success = false;
    event.frame_id = request.frame_id;
    event.error = e.what();
    record_scene_event_locked(std::move(event));
    return core::SceneEditResultDTO{
        false, core::ErrorDTO{core::kErrorInternalError, e.what(), {}}};
  }
}

core::SceneEditResultDTO RoboPlanProjectionBackend::detach_object(
    const core::DetachObjectRequestDTO& request) {
  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    const std::string target_frame = request.frame_id.empty() ? "universe" : request.frame_id;
    const auto it = object_states_.find(request.object_id);
    if (it == object_states_.end()) {
      auto result = core::SceneEditResultDTO{
          false,
          core::ErrorDTO{core::kErrorObjectNotFound,
                         "Object not found: " + request.object_id, {}},
      };
      result.rollback_proven = true;
      return result;
    }
    auto& tracked = it->second;
    const TrackedObjectState state_before = tracked;
    auto clear_result = clear_touch_links_locked(request.object_id, tracked);
    if (!clear_result.has_value()) {
      if (!state_before.touch_links.empty()) {
        publish_collision_world_revision_locked(
            core::CollisionWorldMutationSource::kPolicyUpdate,
            object_body_ids_for_state_locked(request.object_id, tracked),
            "scene_edit.detach_object.partial_failure");
      }
      return core::SceneEditResultDTO{
          false,
          core::ErrorDTO{
              core::kErrorInternalError,
              "Failed to clear touch links before detach: " + clear_result.error(),
              {},
          },
      };
    }
    auto result = reparent_object_locked(request.object_id, target_frame);
    if (result.success) {
      auto& detached_state = object_states_.at(request.object_id);
      detached_state.source_id.reset();
      detached_state.source_timestamp_ns.reset();
      detached_state.source_freshness_budget_ns.reset();
      detached_state.scene_admission_role.reset();
      detached_state.scene_admission_lease_max_age_ns.reset();
      detached_state.scene_admission_target_drift_tolerance_m.reset();
      detached_state.scene_admission_target_angular_tolerance_rad.reset();
      detached_state.scene_admission_obstacle_drift_tolerance_m.reset();
      detached_state.scene_admission_obstacle_angular_tolerance_rad.reset();
      detached_state.scene_admission_allow_runtime_dynamic_obstacle = false;
      publish_collision_world_revision_locked(
          core::CollisionWorldMutationSource::kDetach,
          object_body_ids_for_transition_locked(
              request.object_id, state_before, object_states_.at(request.object_id)),
          "scene_edit.detach_object");
    } else if (state_before.touch_links != tracked.touch_links) {
      publish_collision_world_revision_locked(
          core::CollisionWorldMutationSource::kPolicyUpdate,
          object_body_ids_for_state_locked(request.object_id, tracked),
          "scene_edit.detach_object.partial_failure");
    }
    return result;
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    SceneTraceEvent event;
    event.event = "detach";
    event.object_id = request.object_id;
    event.success = false;
    event.frame_id = request.frame_id.empty()
                         ? std::optional<std::string>("universe")
                         : std::optional<std::string>(request.frame_id);
    event.error = e.what();
    record_scene_event_locked(std::move(event));
    return core::SceneEditResultDTO{
        false, core::ErrorDTO{core::kErrorInternalError, e.what(), {}}};
  }
}

std::vector<RoboPlanProjectionBackend::SceneTraceEvent>
RoboPlanProjectionBackend::get_scene_trace(size_t limit) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  if (limit == 0 || limit >= scene_trace_.size()) {
    return {scene_trace_.begin(), scene_trace_.end()};
  }
  return {scene_trace_.end() - static_cast<std::ptrdiff_t>(limit), scene_trace_.end()};
}

void RoboPlanProjectionBackend::clear_scene_trace() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  scene_trace_.clear();
}

std::map<std::string, std::uint64_t>
RoboPlanProjectionBackend::scene_update_status_counters() const {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  return {
      {"geometry_changed", scene_update_geometry_changed_count_},
      {"freshness_only", scene_update_freshness_only_count_},
      {"no_op", scene_update_no_op_count_},
      {"runtime_bundle_published", runtime_bundle_publish_count_},
      {"runtime_bundle_publish_backpressure",
       runtime_bundle_publish_backpressure_count_},
  };
}

core::SceneProjectionCommitResult
RoboPlanProjectionBackend::rebuild_from_logical_snapshot(
    const core::LogicalSceneSnapshot& snapshot,
    const core::SceneRevisionDTO& revision) {
  core::SceneProjectionCommitResult result;
  result.acknowledged_revision = published_collision_world_revision();
  const auto fail = [&](std::string message, std::string reason,
                        bool rollback_proven = true) {
    result.edit.success = false;
    result.edit.error = core::ErrorDTO{
        core::kErrorInvariantViolation, std::move(message),
        {{"reason", std::move(reason)}}};
    result.rollback_proven = rollback_proven;
    return result;
  };

  if (revision.revision_id == 0 || revision.content_hash.empty() ||
      snapshot.revision.revision_id != revision.revision_id ||
      snapshot.revision.content_hash != revision.content_hash) {
    return fail("Projection rebuild requires one valid committed revision",
                "projection_rebuild_revision_invalid");
  }
  if (snapshot.world_frame.empty()) {
    return fail("Projection rebuild world frame must not be empty",
                "projection_rebuild_world_frame_empty");
  }

  std::shared_ptr<roboplan::Scene> candidate_scene;
  Eigen::VectorXd current_positions;
  Eigen::Matrix4d world_from_robot_base;
  std::string robot_id;
  try {
    {
      std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
      candidate_scene = std::make_shared<roboplan::Scene>(*base_scene_);
      current_positions = scene_->getCurrentJointPositions();
    }
    {
      std::lock_guard<std::mutex> mapper_lock(mapper_mutex_);
      robot_id = mapper_robot_id_;
    }
    const auto placement = std::find_if(
        snapshot.robot_placements.begin(), snapshot.robot_placements.end(),
        [&](const auto& candidate) {
          return candidate.robot_id == robot_id;
        });
    if (placement == snapshot.robot_placements.end() ||
        snapshot.robot_placements.size() != 1) {
      return fail(
          "RoboPlan projection rebuild requires exactly its qualified robot placement",
          "projection_rebuild_robot_placement_invalid");
    }
    world_from_robot_base = pose_to_matrix(placement->pose_in_world);
    candidate_scene->setJointPositions(current_positions);
  } catch (const std::exception& error) {
    return fail("Failed to initialize a clean RoboPlan projection candidate: " +
                    std::string(error.what()),
                "projection_rebuild_candidate_initialization_failed");
  }

  auto candidate_mutex = std::make_shared<std::mutex>();
  auto candidate = std::make_unique<RoboPlanProjectionBackend>(
      candidate_scene, group_name_, candidate_mutex,
      snapshot.configured_collision_policy, robot_id);
  {
    std::lock_guard<std::mutex> candidate_lock(*candidate_mutex);
    candidate->world_from_robot_base_ = world_from_robot_base;
  }
  for (const auto& object : snapshot.objects) {
    core::AddObjectRequestDTO add;
    add.object_id = object.object_id;
    add.shape = object.shape;
    add.pose = object.pose_in_frame;
    add.frame_id = object.frame_id;
    add.support_contacts = object.support_contacts;
    auto added = candidate->add_object(add);
    if (!added.success) {
      return fail("Failed to rebuild RoboPlan object '" + object.object_id +
                      "': " +
                      (added.error ? added.error->message : "unknown error"),
                  "projection_rebuild_object_add_failed");
    }

    if (object.attached || !object.touch_links.empty()) {
      core::AttachObjectRequestDTO attach;
      attach.object_id = object.object_id;
      attach.frame_id = object.frame_id;
      attach.touch_links = object.touch_links;
      auto attached = candidate->attach_object(attach);
      if (!attached.success) {
        return fail("Failed to rebuild RoboPlan attachment for '" +
                        object.object_id + "': " +
                        (attached.error ? attached.error->message
                                        : "unknown error"),
                    "projection_rebuild_object_attach_failed");
      }
    }

    const bool has_observation_metadata =
        object.source_id.has_value() ||
        object.source_timestamp_ns.has_value() ||
        object.source_freshness_budget_ns.has_value() ||
        object.scene_admission_role.has_value() ||
        object.scene_admission_lease_max_age_ns.has_value() ||
        object.scene_admission_target_drift_tolerance_m.has_value() ||
        object.scene_admission_target_angular_tolerance_rad.has_value() ||
        object.scene_admission_obstacle_drift_tolerance_m.has_value() ||
        object.scene_admission_obstacle_angular_tolerance_rad.has_value() ||
        object.scene_admission_allow_runtime_dynamic_obstacle;
    if (has_observation_metadata) {
      core::UpdateObjectPoseRequestDTO update;
      update.object_id = object.object_id;
      update.pose = object.pose_in_frame;
      update.frame_id = object.frame_id;
      update.source_id = object.source_id;
      update.source_timestamp_ns = object.source_timestamp_ns;
      update.source_freshness_budget_ns =
          object.source_freshness_budget_ns;
      if (object.scene_admission_role.has_value()) {
        update.scene_admission_role =
            core::to_string(*object.scene_admission_role);
      }
      update.scene_admission_lease_max_age_ns =
          object.scene_admission_lease_max_age_ns;
      update.scene_admission_target_drift_tolerance_m =
          object.scene_admission_target_drift_tolerance_m;
      update.scene_admission_target_angular_tolerance_rad =
          object.scene_admission_target_angular_tolerance_rad;
      update.scene_admission_obstacle_drift_tolerance_m =
          object.scene_admission_obstacle_drift_tolerance_m;
      update.scene_admission_obstacle_angular_tolerance_rad =
          object.scene_admission_obstacle_angular_tolerance_rad;
      update.scene_admission_allow_runtime_dynamic_obstacle =
          object.scene_admission_allow_runtime_dynamic_obstacle;
      auto updated = candidate->update_object_pose(update);
      if (!updated.success) {
        return fail("Failed to rebuild RoboPlan observation metadata for '" +
                        object.object_id + "': " +
                        (updated.error ? updated.error->message
                                       : "unknown error"),
                    "projection_rebuild_object_metadata_failed");
      }
    }
  }

  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    candidate_scene->setJointPositions(scene_->getCurrentJointPositions());
    *scene_ = *candidate_scene;
    object_states_ = std::move(candidate->object_states_);
    object_body_hash_cache_.clear();
    disabled_static_collision_pairs_ =
        std::move(candidate->disabled_static_collision_pairs_);
    disabled_support_contact_pairs_ =
        std::move(candidate->disabled_support_contact_pairs_);
    world_from_robot_base_ = candidate->world_from_robot_base_;
    // Existing retained projections are immutable clones of the last
    // committed revision. Rebuilding the live projection must not invalidate
    // in-flight leases that SceneAuthority still owns.
    {
      std::lock_guard<std::mutex> mapper_lock(mapper_mutex_);
      mapper_scene_fingerprint_.clear();
    }
    world_revision_id_ = revision.revision_id;
    topology_epoch_ = snapshot.topology_epoch;
    policy_epoch_ = snapshot.policy_epoch;
    snapshot_cache_.reset();
    snapshot_cache_revision_id_ = 0;
    snapshot_cache_content_hash_.clear();
    publish_collision_world_revision_locked(
        core::CollisionWorldMutationSource::kProjectionRebuild, {},
        "scene_projection.rebuild", std::nullopt,
        /*advance_revision=*/false, /*advance_epochs=*/false);
    result.acknowledged_revision =
        core::CollisionWorldRevision{world_revision_id_, world_content_hash_};
  } catch (const std::exception& error) {
    return fail("Failed to install the rebuilt RoboPlan projection: " +
                    std::string(error.what()),
                "projection_rebuild_install_failed",
                /*rollback_proven=*/false);
  }

  if (result.acknowledged_revision.revision_id != revision.revision_id ||
      result.acknowledged_revision.content_hash != revision.content_hash) {
    return fail("Rebuilt RoboPlan projection content does not match the "
                "committed logical revision",
                "projection_rebuild_content_hash_mismatch",
                /*rollback_proven=*/false);
  }
  auto runtime = published_runtime_collision_query_bundle();
  if (!runtime || !runtime->world ||
      runtime->world->revision.revision_id != revision.revision_id ||
      runtime->world->revision.content_hash != revision.content_hash) {
    return fail("Rebuilt RoboPlan runtime publication does not match the "
                "committed logical revision",
                "projection_rebuild_runtime_publication_mismatch",
                /*rollback_proven=*/false);
  }

  result.edit.success = true;
  result.edit.update_status = "geometry_changed";
  result.edit.collision_world_revision_changed = true;
  result.rollback_proven = true;
  return result;
}

core::Result<core::CollisionWorldLeaseInventory>
RoboPlanProjectionBackend::retain_planner_projection(
    const core::CollisionWorldRevision& revision,
    const std::string& retention_id) {
  if (retention_id.empty()) {
    return core::Result<core::CollisionWorldLeaseInventory>::Err(
        core::ErrorDTO{core::kErrorInvalidRequest,
                       "Planner projection retention id must not be empty",
                       {{"reason", "projection_retention_id_empty"}}});
  }
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  if (revision.revision_id != world_revision_id_ ||
      revision.content_hash != world_content_hash_) {
    return core::Result<core::CollisionWorldLeaseInventory>::Err(
        core::ErrorDTO{
            core::kErrorSceneChangedDuringValidation,
            "Requested planner projection revision is not current",
            {{"reason", "planner_projection_revision_unavailable"},
             {"requested_revision_id",
              std::to_string(revision.revision_id)},
             {"current_revision_id", std::to_string(world_revision_id_)}}});
  }
  const auto existing = retained_planner_projections_.find(retention_id);
  bool inserted = false;
  if (existing != retained_planner_projections_.end()) {
    if (existing->second.revision.revision_id != revision.revision_id ||
        existing->second.revision.content_hash != revision.content_hash) {
      return core::Result<core::CollisionWorldLeaseInventory>::Err(
          core::ErrorDTO{
              core::kErrorInvariantViolation,
              "Planner projection retention id was reused for another revision",
              {{"reason", "projection_retention_id_reused"}}});
    }
  } else {
    if (retained_planner_projections_.size() >=
        kPlannerProjectionRetentionCapacity) {
      return core::Result<core::CollisionWorldLeaseInventory>::Err(
          core::ErrorDTO{
              core::kErrorExecutionRejected,
              "Planner projection retention capacity is exhausted",
              {{"reason", "projection_retention_capacity_exhausted"}}});
    }
    std::map<std::string, core::Pose3DTO> object_world_poses;
    for (const auto& object : scene_objects_locked()) {
      object_world_poses.emplace(object.object_id, object.pose_in_universe);
    }
    retained_planner_projections_.emplace(
        retention_id,
        RetainedPlannerProjection{
            revision,
            std::make_shared<roboplan::Scene>(*scene_),
            matrix_to_pose(world_from_robot_base_),
            std::move(object_world_poses)});
    inserted = true;
  }

  std::string robot_id;
  {
    std::scoped_lock map_lock(mapper_mutex_);
    robot_id = mapper_robot_id_;
  }
  auto inventory = build_scene_snapshot_lease_inventory_locked(
      robot_id, retention_id,
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  if (!inventory.bundle.world || !inventory.bundle.policy.has_value() ||
      inventory.bundle.world->revision.revision_id != revision.revision_id ||
      inventory.bundle.world->revision.content_hash != revision.content_hash) {
    if (inserted) {
      retained_planner_projections_.erase(retention_id);
    }
    return core::Result<core::CollisionWorldLeaseInventory>::Err(
        core::ErrorDTO{
            core::kErrorInvariantViolation,
            "Planner projection retention did not capture a coherent world and policy",
            {{"reason", "planner_projection_inventory_incoherent"},
             {"projection_lease_id", retention_id}}});
  }
  return core::Result<core::CollisionWorldLeaseInventory>::Ok(
      std::move(inventory));
}

std::shared_ptr<roboplan::Scene>
RoboPlanProjectionBackend::acquire_planner_projection(
    const core::CollisionWorldRevision& revision,
    const std::string& retention_id) const {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  const auto it = retained_planner_projections_.find(retention_id);
  if (it == retained_planner_projections_.end() ||
      it->second.revision.revision_id != revision.revision_id ||
      it->second.revision.content_hash != revision.content_hash) {
    return nullptr;
  }
  return it->second.scene;
}

core::Result<core::Pose3DTO>
RoboPlanProjectionBackend::resolve_reference_pose_on_retained_projection(
    const core::ReferenceDTO& reference, const std::string& base_frame,
    const core::SceneRevisionBindingDTO& binding) const {
  if (!binding.valid()) {
    return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
        core::kErrorInvalidRequest,
        "Reference resolution requires a valid retained scene binding",
        {{"reason", "reference_projection_binding_invalid"}}});
  }

  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  const auto retained =
      retained_planner_projections_.find(binding.projection_lease_id);
  if (retained == retained_planner_projections_.end() ||
      retained->second.revision.revision_id != binding.revision.revision_id ||
      retained->second.revision.content_hash != binding.revision.content_hash ||
      !retained->second.scene) {
    return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
        core::kErrorSceneChangedDuringValidation,
        "Retained scene projection is unavailable for reference resolution",
        {{"reason", "reference_projection_lease_unavailable"},
         {"projection_lease_id", binding.projection_lease_id}}});
  }

  try {
    const auto& projection = retained->second;
    const Eigen::Matrix4d world_from_robot_base =
        pose_to_matrix(projection.robot_base_pose_in_world);
    const auto frame_to_world =
        [&](const std::string& frame_id) -> Eigen::Matrix4d {
      if (is_world_frame(frame_id)) {
        return Eigen::Matrix4d::Identity();
      }
      const auto& positions = projection.scene->getCurrentJointPositions();
      Eigen::VectorXd q(positions.size());
      for (Eigen::Index index = 0; index < positions.size(); ++index) {
        q[index] = positions[index];
      }
      return world_from_robot_base *
             projection.scene->forwardKinematics(q, frame_id);
    };

    std::map<std::string, std::string> details{
        {"reference_id", reference.id.empty() ? "inline_pose" : reference.id},
        {"projection_lease_id", binding.projection_lease_id},
        {"scene_revision_id", std::to_string(binding.revision.revision_id)}};
    switch (reference.type) {
      case core::ReferenceDTO::Type::kObject: {
        details["reference_type"] = "object";
        const auto object = projection.object_world_poses.find(reference.id);
        if (object == projection.object_world_poses.end()) {
          return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
              core::kErrorMissingReference,
              "Object reference is absent from the retained scene",
              std::move(details)});
        }
        const auto base_from_world = frame_to_world(base_frame).inverse();
        return core::Result<core::Pose3DTO>::Ok(matrix_to_pose(
            base_from_world * pose_to_matrix(object->second)));
      }
      case core::ReferenceDTO::Type::kFrame:
      case core::ReferenceDTO::Type::kRobotLink: {
        details["reference_type"] =
            reference.type == core::ReferenceDTO::Type::kFrame
                ? "frame"
                : "robot_link";
        if (reference.id.empty()) {
          return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
              core::kErrorMissingReference,
              "Reference id is empty",
              std::move(details)});
        }
        return core::Result<core::Pose3DTO>::Ok(matrix_to_pose(
            frame_to_world(base_frame).inverse() *
            frame_to_world(reference.id)));
      }
      case core::ReferenceDTO::Type::kPose:
        details["reference_type"] = "pose";
        if (!reference.pose.has_value()) {
          return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
              core::kErrorMissingReference,
              "Pose reference is missing inline pose data",
              std::move(details)});
        }
        return core::Result<core::Pose3DTO>::Ok(*reference.pose);
    }
  } catch (const std::exception& error) {
    return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
        core::kErrorMissingReference,
        "Reference could not be resolved in the retained scene projection",
        {{"reason", "reference_projection_resolution_failed"},
         {"reference_id", reference.id},
         {"projection_lease_id", binding.projection_lease_id},
         {"exception", error.what()}}});
  }

  return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
      core::kErrorInvalidRequest,
      "Unsupported reference type",
      {{"reason", "reference_type_unsupported"}}});
}

void RoboPlanProjectionBackend::release_planner_projection(
    const std::string& retention_id) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  retained_planner_projections_.erase(retention_id);
}

}  // namespace manyforge::roboplan_backend
