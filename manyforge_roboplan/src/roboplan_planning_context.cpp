#include <manyforge_roboplan/roboplan_planning_context.hpp>

#include <manyforge_roboplan/collision_body_id_mapper.hpp>
#include <manyforge_roboplan/collision_diagnostics_capture.hpp>

#include <Eigen/Dense>
#include <algorithm>
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

RoboPlanPlanningContext::RoboPlanPlanningContext(
    std::shared_ptr<roboplan::Scene> scene, std::string group_name,
    std::shared_ptr<std::mutex> scene_mutex)
    : scene_(std::move(scene)),
      group_name_(std::move(group_name)),
      scene_mutex_(std::move(scene_mutex)),
      mapper_(std::make_unique<RoboPlanCollisionBodyIdMapper>()) {
  if (!scene_mutex_) {
    scene_mutex_ = std::make_shared<std::mutex>();
  }
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  publish_collision_world_revision_locked(
      core::CollisionWorldMutationSource::kProgramLoad,
      {},
      "roboplan_context.constructor");
}

RoboPlanPlanningContext::~RoboPlanPlanningContext() = default;

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
    const std::vector<RoboPlanPlanningContext::SceneObjectSnapshot>& objects,
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

void RoboPlanPlanningContext::set_collision_mapping_robot_id(
    const std::string& robot_id) {
  bool changed = false;
  {
    std::scoped_lock lock(mapper_mutex_);
    if (robot_id != mapper_robot_id_) {
      mapper_robot_id_ = robot_id;
      // Force a rebuild on next collision_check_normalized call.
      mapper_scene_fingerprint_.clear();
      changed = true;
    }
  }
  if (changed) {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    publish_collision_world_revision_locked(
        core::CollisionWorldMutationSource::kDeploymentLoad,
        {},
        "roboplan_context.set_collision_mapping_robot_id");
  }
}

void RoboPlanPlanningContext::set_collision_policy_snapshot(
    core::CollisionPolicySnapshot snapshot) {
  std::scoped_lock lock(evaluator_mutex_);
  policy_snapshot_ = std::move(snapshot);
}

void RoboPlanPlanningContext::reset_collision_policy_state() {
  // Chunks 3-4 create a query-local evaluator for every normalized check.
  // There is no planning-context-global policy state to reset. The method
  // remains as a stable API for Chunk 5, where the validation coordinator owns
  // trajectory-scoped evaluator state.
}

void RoboPlanPlanningContext::set_collision_engine_for_compare(
    std::shared_ptr<core::ICollisionQueryEngine> engine) {
  std::scoped_lock lock(compare_engine_mutex_);
  compare_engine_ = std::move(engine);
}

void RoboPlanPlanningContext::set_engine_mismatch_record_emitter(
    MismatchRecordEmitter emitter) {
  std::scoped_lock lock(compare_engine_mutex_);
  mismatch_record_emitter_ = std::move(emitter);
}

core::CollisionWorldRevision
RoboPlanPlanningContext::current_collision_world_revision() {
  // Chunk 2: returns the authoritative revision tracked by the in-context
  // publisher. The mapper's local revision is derived from this on the
  // collision_check_normalized path; this method is the single source of
  // truth for the world revision exposed via the IPlanningContext port.
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  return core::CollisionWorldRevision{world_revision_id_, world_content_hash_};
}

core::CollisionWorldSnapshotPtr RoboPlanPlanningContext::snapshot() const {
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
RoboPlanPlanningContext::recent_collision_world_ledger(std::size_t limit) const {
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

void RoboPlanPlanningContext::publish_collision_world_revision_locked(
    core::CollisionWorldMutationSource mutation_source,
    const std::vector<std::string>& affected_body_ids,
    std::string authority_source,
    std::optional<std::string> session_id) {
  // Caller MUST hold scene_mutex_. Build the new revision id + content hash
  // and append a ledger entry. content_hash is a pure digest of the collision
  // world content; revision_id and the ledger fields provide sequence
  // identity for repeated mutations that leave content unchanged.
  const std::uint64_t previous_revision_id = world_revision_id_;
  world_revision_id_ += 1;

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

core::CollisionResult RoboPlanPlanningContext::collision_check_normalized(
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
        result.collision_world_revision = current_collision_world_revision();
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
    // Snapshot the body inventory and run the legacy collision query while
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
  // at planning-context scope; cross-waypoint support-contact state belongs to
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
          "RoboPlanPlanningContext::snapshot() returned null";
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

void RoboPlanPlanningContext::set_robot_base_pose(const core::Pose3DTO& pose) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  world_from_robot_base_ = pose_to_matrix(pose);

  for (const auto& [object_id, tracked] : object_states_) {
    Eigen::Matrix4d t_scene_object = scene_placement_from_tracked_locked(tracked);
    const std::string target_frame =
        is_world_frame(tracked.frame_id) ? "universe" : tracked.frame_id;
    auto r = scene_->updateGeometryPlacement(object_id, target_frame, t_scene_object);
    if (!r.has_value()) {
      throw std::runtime_error(
          "Failed to update object '" + object_id + "' after robot base pose change: " +
          r.error());
    }
  }
  publish_collision_world_revision_locked(
      core::CollisionWorldMutationSource::kDeploymentLoad,
      {},
      "roboplan_context.set_robot_base_pose");
}

core::RobotStateDTO RoboPlanPlanningContext::get_current_state() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  core::RobotStateDTO state;
  const auto maybe_group_info = scene_->getJointGroupInfo(group_name_);
  if (!maybe_group_info) {
    throw std::runtime_error("Failed to get current state: " + maybe_group_info.error());
  }
  const auto& group_info = maybe_group_info.value();
  state.joint_names = group_info.joint_names;

  Eigen::VectorXd positions = scene_->getCurrentJointPositions()(group_info.q_indices);
  if (group_info.has_continuous_dofs) {
    auto maybe_collapsed =
        roboplan::collapseContinuousJointPositions(*scene_, group_name_, positions);
    if (!maybe_collapsed) {
      throw std::runtime_error("Failed to get current state: " + maybe_collapsed.error());
    }
    positions = maybe_collapsed.value();
  }

  state.positions_rad.resize(positions.size());
  for (Eigen::Index i = 0; i < positions.size(); ++i) {
    state.positions_rad[i] = positions[i];
  }

  state.velocities_rad_s.assign(state.positions_rad.size(), 0.0);
  return state;
}

core::ValidationResultDTO RoboPlanPlanningContext::validate_trajectory(
    const core::TrajectoryDTO& trajectory,
    const core::ValidationOptionsDTO& options) {
  core::ValidationResultDTO result;

  try {
    const size_t waypoint_count = trajectory.waypoints.size();
    const double trajectory_duration_s =
        trajectory.waypoints.empty()
            ? 0.0
            : trajectory.waypoints.back().time_from_start_s;
    uint64_t speed_validation_duration_ns = 0;
    uint64_t collision_validation_duration_ns = 0;

    // TCP speed validation is intentionally first. It is cheap FK-only work and
    // can reject infeasible trajectories before expensive mesh collision sweeps.
    //
    // Lock scope: held once across the whole speed-validation loop rather
    // than per waypoint. The previous per-iteration acquire/release was the
    // worst-of-both-worlds: it imposed lock-thrash overhead while still
    // holding the scene mutex cumulatively for the same duration (FK can
    // only run while we hold the mutex because Pinocchio's `Data` inside
    // `roboplan::Scene` is non-reentrant — see ADR-0019). With FK at
    // microsecond scale per waypoint, a single acquire is both faster and
    // gives concurrent get_status() callers a single contiguous wait
    // window instead of N short ones.
    if (options.max_tcp_linear_speed_mps > 0.0 &&
        !options.tcp_frame.empty() && waypoint_count >= 2) {
      const uint64_t speed_start_ns = steady_now_ns();
      Eigen::Vector3d prev_pos = Eigen::Vector3d::Zero();
      bool have_prev = false;
      double max_speed = 0.0;
      size_t max_segment_index = 0;
      double max_segment_dt_s = 0.0;

      {
        std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
        for (size_t i = 0; i < waypoint_count; ++i) {
          const auto& wp = trajectory.waypoints[i];
          Eigen::VectorXd q = vector_from_std(wp.positions_rad);
          if (q.size() != scene_->getCurrentJointPositions().size()) {
            q = scene_->toFullJointPositions(group_name_, q);
          }

          Eigen::Matrix4d tform = scene_->forwardKinematics(q, options.tcp_frame);
          Eigen::Vector3d pos = tform.block<3, 1>(0, 3);

          if (have_prev && i > 0) {
            const double dt =
                wp.time_from_start_s - trajectory.waypoints[i - 1].time_from_start_s;
            if (dt > 1e-9) {
              const double speed = (pos - prev_pos).norm() / dt;
              if (speed > max_speed) {
                max_speed = speed;
                max_segment_index = i - 1;
                max_segment_dt_s = dt;
              }
            }
          }

          prev_pos = pos;
          have_prev = true;
        }
      }
      speed_validation_duration_ns = steady_now_ns() - speed_start_ns;

      if (max_speed > options.max_tcp_linear_speed_mps) {
        std::map<std::string, std::string> details;
        details["limit_type"] = "tcp_linear_speed";
        details["measured"] = std::to_string(max_speed);
        details["limit"] = std::to_string(options.max_tcp_linear_speed_mps);
        details["units"] = "m/s";
        details["tcp_frame"] = options.tcp_frame;
        details["waypoint_count"] = std::to_string(waypoint_count);
        details["trajectory_duration_s"] = std::to_string(trajectory_duration_s);
        details["max_segment_index"] = std::to_string(max_segment_index);
        details["max_segment_dt_s"] = std::to_string(max_segment_dt_s);
        details["speed_validation_duration_ns"] =
            std::to_string(speed_validation_duration_ns);
        details["collision_validation_duration_ns"] = "0";
        result.error = core::ErrorDTO{
            core::kErrorValidationLimitsViolation,
            "TCP linear speed exceeds limit", details};
        return result;
      }
    }

    if (!options.run_collision_checks) {
      result.valid = true;
      return result;
    }

    // Collision check at each waypoint.
    //
    // Support contacts emulate a MoveIt-style per-request ACM in the RoboPlan
    // adapter. Declared support pairs are disabled in the planner projection so
    // RRT can start from a resting grasp, but validation temporarily re-enables
    // them and enforces release semantics over the generated trajectory.
    const uint64_t collision_start_ns = steady_now_ns();
    std::set<std::pair<std::string, std::string>> start_support_pairs;
    std::set<std::pair<std::string, std::string>> separated_support_pairs;
    bool has_support_contact_pairs = false;
    {
      std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
      has_support_contact_pairs = !disabled_support_contact_pairs_.empty();
    }

    for (size_t i = 0; i < waypoint_count; ++i) {
      const auto& wp = trajectory.waypoints[i];
      bool waypoint_in_collision = false;
      CollisionDiagnostics diagnostics;
      std::vector<CollisionPairDiagnostic> blocking_pairs;
      std::set<std::pair<std::string, std::string>> colliding_support_pairs;

      {
        std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
        bool support_contacts_exposed = false;
        auto restore_support_contacts = [&]() {
          if (!support_contacts_exposed) {
            return;
          }
          (void)set_support_contact_pairs_for_validation_locked(false);
          support_contacts_exposed = false;
        };
        const auto support_guard =
            std::shared_ptr<void>(nullptr, [&](void*) { restore_support_contacts(); });
        (void)support_guard;
        if (has_support_contact_pairs) {
          auto expose_support_contacts =
              set_support_contact_pairs_for_validation_locked(true);
          if (!expose_support_contacts.has_value()) {
            result.error = core::ErrorDTO{
                core::kErrorInternalError,
                "Failed to expose support-contact pairs for validation: " +
                    expose_support_contacts.error(),
                {},
            };
            return result;
          }
          support_contacts_exposed = true;
        }

        Eigen::VectorXd q = vector_from_std(wp.positions_rad);
        if (q.size() != scene_->getCurrentJointPositions().size()) {
          q = scene_->toFullJointPositions(group_name_, q);
        }
        waypoint_in_collision = scene_->hasCollisions(q);
        if (waypoint_in_collision) {
          diagnostics = collect_collision_diagnostics_locked(q);
          for (const auto& pair : diagnostics.pairs) {
            const auto support_pair =
                pair.canonical_object_pair.value_or(pair.canonical_pair);
            if (has_support_contact_pairs &&
                is_active_support_contact_pair_locked(support_pair)) {
              colliding_support_pairs.insert(support_pair);
            } else {
              blocking_pairs.push_back(pair);
            }
          }
        }
      }
      // Collision validation can run across many waypoints and each waypoint
      // needs the non-reentrant RoboPlan/Pinocchio scene. Yield between
      // waypoint checks so observer FK and scene-state reads are not starved
      // behind a tight validation loop for the full validation duration.
      std::this_thread::yield();

      if (!waypoint_in_collision) {
        if (i > 0) {
          for (const auto& pair : start_support_pairs) {
            separated_support_pairs.insert(pair);
          }
        }
        continue;
      }

      if (diagnostics.pairs.empty()) {
        collision_validation_duration_ns = steady_now_ns() - collision_start_ns;
        std::map<std::string, std::string> details;
        details["failure_stage"] = "collision_validation_unstructured";
        details["waypoint_index"] = std::to_string(i);
        details["waypoint_count"] = std::to_string(waypoint_count);
        details["trajectory_duration_s"] = std::to_string(trajectory_duration_s);
        details["speed_validation_duration_ns"] =
            std::to_string(speed_validation_duration_ns);
        details["collision_validation_duration_ns"] =
            std::to_string(collision_validation_duration_ns);
        details["collision_pair_count"] = "0";
        details["collision_diagnostics_available"] = "false";
        result.error = core::ErrorDTO{
            core::kErrorValidationCollision,
            "Trajectory passes through collision, but RoboPlan did not report collision pairs",
            details,
        };
        return result;
      }

      auto fail_with_diagnostics = [&](const std::string& stage,
                                       const std::string& message) {
        collision_validation_duration_ns = steady_now_ns() - collision_start_ns;
        std::map<std::string, std::string> details;
        details["failure_stage"] = stage;
        details["waypoint_index"] = std::to_string(i);
        details["waypoint_count"] = std::to_string(waypoint_count);
        details["trajectory_duration_s"] = std::to_string(trajectory_duration_s);
        details["speed_validation_duration_ns"] =
            std::to_string(speed_validation_duration_ns);
        details["collision_validation_duration_ns"] =
            std::to_string(collision_validation_duration_ns);
        add_collision_details(details, diagnostics);
        result.error = core::ErrorDTO{
            core::kErrorValidationCollision,
            message,
            details,
        };
      };

      if (!blocking_pairs.empty()) {
        fail_with_diagnostics(
            "collision_validation",
            "Trajectory passes through collision");
        return result;
      }

      if (i == 0) {
        start_support_pairs = colliding_support_pairs;
        continue;
      }

      for (const auto& pair : colliding_support_pairs) {
        if (start_support_pairs.find(pair) == start_support_pairs.end()) {
          fail_with_diagnostics(
              "support_contact_unexpected_path_contact",
              "Support-contact pair collided after the start waypoint");
          return result;
        }
        if (separated_support_pairs.find(pair) != separated_support_pairs.end()) {
          fail_with_diagnostics(
              "support_contact_recontact_after_release",
              "Support-contact pair re-contacted after separation");
          return result;
        }
      }

      for (const auto& pair : start_support_pairs) {
        if (colliding_support_pairs.find(pair) == colliding_support_pairs.end()) {
          separated_support_pairs.insert(pair);
        }
      }
    }

    for (const auto& pair : start_support_pairs) {
      std::optional<core::SupportContactDTO> policy;
      {
        std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
        policy = support_contact_policy_for_pair_locked(pair);
      }
        const bool require_separation =
            !policy.has_value() || policy->require_separation_by_end;
        if (!require_separation ||
            separated_support_pairs.find(pair) != separated_support_pairs.end()) {
          continue;
        }
        collision_validation_duration_ns = steady_now_ns() - collision_start_ns;
        std::map<std::string, std::string> details;
        details["failure_stage"] = "support_contact_not_released";
        details["support_contact_pair"] = collision_pair_text(pair);
        details["waypoint_count"] = std::to_string(waypoint_count);
        details["trajectory_duration_s"] = std::to_string(trajectory_duration_s);
        details["speed_validation_duration_ns"] =
            std::to_string(speed_validation_duration_ns);
        details["collision_validation_duration_ns"] =
            std::to_string(collision_validation_duration_ns);
        if (policy.has_value()) {
          details["enforce_max_start_penetration"] =
              policy->enforce_max_start_penetration ? "true" : "false";
          details["max_start_penetration_m"] =
              std::to_string(policy->max_start_penetration_m);
          details["support_contact_depth_enforced"] = "false";
        }
        result.error = core::ErrorDTO{
            core::kErrorValidationCollision,
            "Support-contact pair did not separate by final waypoint",
            details,
        };
        return result;
      }
    collision_validation_duration_ns = steady_now_ns() - collision_start_ns;

    result.valid = true;
  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorInternalError, e.what(), {}};
  }

  return result;
}

core::Result<core::TrajectoryDTO>
RoboPlanPlanningContext::expand_trajectory_for_collision_validation(
    const core::TrajectoryDTO& trajectory) {
  if (trajectory.waypoints.empty()) {
    return core::Result<core::TrajectoryDTO>::Ok(trajectory);
  }

  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    const auto current_q = scene_->getCurrentJointPositions();
    const auto full_q_size = static_cast<std::size_t>(current_q.size());
    if (full_q_size == 0) {
      return core::Result<core::TrajectoryDTO>::Ok(trajectory);
    }

    bool positions_already_full = true;
    for (const auto& waypoint : trajectory.waypoints) {
      if (waypoint.positions_rad.size() != full_q_size) {
        positions_already_full = false;
        break;
      }
    }

    const auto& model = scene_->getModel();
    std::vector<std::string> full_joint_names(full_q_size);
    for (pinocchio::JointIndex joint_id = 1;
         joint_id < static_cast<pinocchio::JointIndex>(model.names.size());
         ++joint_id) {
      if (joint_id >= static_cast<pinocchio::JointIndex>(model.nqs.size()) ||
          joint_id >= static_cast<pinocchio::JointIndex>(model.idx_qs.size()) ||
          model.nqs[joint_id] != 1) {
        continue;
      }
      const auto q_index = model.idx_qs[joint_id];
      if (q_index >= 0 && static_cast<std::size_t>(q_index) < full_q_size) {
        full_joint_names[static_cast<std::size_t>(q_index)] =
            model.names[static_cast<std::size_t>(joint_id)];
      }
    }
    for (std::size_t i = 0; i < full_joint_names.size(); ++i) {
      if (full_joint_names[i].empty()) {
        full_joint_names[i] = "q[" + std::to_string(i) + "]";
      }
    }

    if (positions_already_full) {
      auto expanded = trajectory;
      expanded.joint_names = std::move(full_joint_names);
      return core::Result<core::TrajectoryDTO>::Ok(std::move(expanded));
    }

    const auto maybe_group_info = scene_->getJointGroupInfo(group_name_);
    if (!maybe_group_info) {
      return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
          core::kErrorValidationCollision,
          "Failed to expand trajectory for collision validation",
          {{"reason", "active_group_unavailable"},
           {"group", group_name_},
           {"detail", maybe_group_info.error()}}});
    }
    const auto& group_info = *maybe_group_info;
    if (group_info.joint_names.size() !=
        static_cast<std::size_t>(group_info.q_indices.size())) {
      return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
          core::kErrorValidationCollision,
          "Failed to expand trajectory for collision validation",
          {{"reason", "active_group_contains_mimic_or_fixed_entries"},
           {"group", group_name_}}});
    }

    std::map<std::string, Eigen::Index> q_index_by_joint;
    for (std::size_t i = 0; i < group_info.joint_names.size(); ++i) {
      const auto q_index = group_info.q_indices[static_cast<Eigen::Index>(i)];
      if (q_index < 0 || q_index >= current_q.size()) {
        return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
            core::kErrorValidationCollision,
            "Failed to expand trajectory for collision validation",
            {{"reason", "active_group_q_index_out_of_range"},
             {"group", group_name_},
             {"joint", group_info.joint_names[i]}}});
      }
      q_index_by_joint[group_info.joint_names[i]] = q_index;
    }

    core::TrajectoryDTO expanded;
    expanded.joint_names = std::move(full_joint_names);
    expanded.waypoints.reserve(trajectory.waypoints.size());
    for (const auto& waypoint : trajectory.waypoints) {
      if (waypoint.positions_rad.size() != trajectory.joint_names.size()) {
        return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
            core::kErrorValidationCollision,
            "Failed to expand trajectory for collision validation",
            {{"reason", "waypoint_position_count_mismatch"},
             {"joint_count", std::to_string(trajectory.joint_names.size())},
             {"position_count",
              std::to_string(waypoint.positions_rad.size())}}});
      }

      auto expanded_waypoint = waypoint;
      expanded_waypoint.positions_rad.resize(full_q_size);
      for (std::size_t i = 0; i < full_q_size; ++i) {
        expanded_waypoint.positions_rad[i] = current_q[static_cast<Eigen::Index>(i)];
      }
      for (std::size_t i = 0; i < trajectory.joint_names.size(); ++i) {
        const auto index_it = q_index_by_joint.find(trajectory.joint_names[i]);
        if (index_it == q_index_by_joint.end()) {
          return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
              core::kErrorValidationCollision,
              "Failed to expand trajectory for collision validation",
              {{"reason", "trajectory_joint_not_in_active_group"},
               {"group", group_name_},
               {"joint", trajectory.joint_names[i]}}});
        }
        expanded_waypoint.positions_rad[static_cast<std::size_t>(index_it->second)] =
            waypoint.positions_rad[i];
      }
      expanded.waypoints.push_back(std::move(expanded_waypoint));
    }
    return core::Result<core::TrajectoryDTO>::Ok(std::move(expanded));
  } catch (const std::exception& e) {
    return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
        core::kErrorValidationCollision,
        "Failed to expand trajectory for collision validation",
        {{"reason", "exception"}, {"exception", e.what()}}});
  }
}

core::CollisionCheckResultDTO RoboPlanPlanningContext::collision_check(
    const core::CollisionCheckRequestDTO& request) {
  core::CollisionCheckResultDTO result;

  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  Eigen::VectorXd q = vector_from_std(request.robot_state.positions_rad);
  if (q.size() != scene_->getCurrentJointPositions().size()) {
    q = scene_->toFullJointPositions(group_name_, q);
  }
  result.in_collision = scene_->hasCollisions(q);
  if (result.in_collision) {
    auto diagnostics = collect_collision_diagnostics_locked(q);
    if (!diagnostics.pairs.empty()) {
      result.contact_body_a = diagnostics.pairs.front().canonical_pair.first;
      result.contact_body_b = diagnostics.pairs.front().canonical_pair.second;
    }
  }

  return result;
}

core::Pose3DTO RoboPlanPlanningContext::forward_kinematics(
    const core::FkRequestDTO& request) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  Eigen::VectorXd q = vector_from_std(request.robot_state.positions_rad);
  if (q.size() != scene_->getCurrentJointPositions().size()) {
    q = scene_->toFullJointPositions(group_name_, q);
  }

  const Eigen::Matrix4d t_robot_base_target =
      is_world_frame(request.target_frame)
          ? Eigen::Matrix4d::Identity()
          : scene_->forwardKinematics(q, request.target_frame);
  if (is_world_frame(request.base_frame)) {
    return matrix_to_pose(world_from_robot_base_ * t_robot_base_target);
  }

  const Eigen::Matrix4d t_robot_base_requested_base =
      scene_->forwardKinematics(q, request.base_frame);
  return matrix_to_pose(t_robot_base_requested_base.inverse() * t_robot_base_target);
}

std::map<std::string, core::Pose3DTO>
RoboPlanPlanningContext::forward_kinematics_batch(
    const core::RobotStateDTO& robot_state,
    const std::vector<std::string>& target_frames,
    const std::string& base_frame) {
  std::map<std::string, core::Pose3DTO> result;
  if (target_frames.empty()) {
    return result;
  }

  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  Eigen::VectorXd q = vector_from_std(robot_state.positions_rad);
  if (q.size() != scene_->getCurrentJointPositions().size()) {
    q = scene_->toFullJointPositions(group_name_, q);
  }

  Eigen::Matrix4d t_requested_base_robot_base = Eigen::Matrix4d::Identity();
  if (is_world_frame(base_frame)) {
    t_requested_base_robot_base = world_from_robot_base_;
  } else {
    const Eigen::Matrix4d t_robot_base_requested_base =
        scene_->forwardKinematics(q, base_frame);
    t_requested_base_robot_base = t_robot_base_requested_base.inverse();
  }

  for (const auto& target_frame : target_frames) {
    try {
      const Eigen::Matrix4d t_robot_base_target =
          is_world_frame(target_frame)
              ? Eigen::Matrix4d::Identity()
              : scene_->forwardKinematics(q, target_frame);
      result[target_frame] =
          matrix_to_pose(t_requested_base_robot_base * t_robot_base_target);
    } catch (...) {
      // Keep observer reads best-effort: one unresolved visual link must not
      // fail the entire native viewer update.
    }
  }
  return result;
}

core::Pose3DTO RoboPlanPlanningContext::get_object_pose(
    const std::string& object_id, const std::string& base_frame) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  const auto it = object_states_.find(object_id);
  if (it == object_states_.end()) {
    throw std::runtime_error("Object not found: " + object_id);
  }

  Eigen::Matrix4d t_frame_object = pose_to_matrix(it->second.pose);
  Eigen::Matrix4d t_world_frame = frame_to_world_matrix_locked(it->second.frame_id);
  Eigen::Matrix4d t_world_object = t_world_frame * t_frame_object;
  Eigen::Matrix4d t_world_base = frame_to_world_matrix_locked(base_frame);
  Eigen::Matrix4d t_base_object = t_world_base.inverse() * t_world_object;
  return matrix_to_pose(t_base_object);
}

core::ObjectStateDTO RoboPlanPlanningContext::get_object_state(
    const std::string& object_id) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  core::ObjectStateDTO state;
  state.object_id = object_id;

  const auto it = object_states_.find(object_id);
  if (it == object_states_.end()) {
    return state;
  }

  state.exists = true;
  state.attached = it->second.attached;
  state.frame_id = it->second.frame_id;
  return state;
}

core::Result<core::Pose3DTO> RoboPlanPlanningContext::resolve_reference_pose(
    const core::ReferenceDTO& reference, const std::string& base_frame) {
  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    std::map<std::string, std::string> details;
    details["reference_id"] = reference.id.empty() ? "inline_pose" : reference.id;

    switch (reference.type) {
      case core::ReferenceDTO::Type::kObject: {
        details["reference_type"] = "object";
        const auto it = object_states_.find(reference.id);
        if (it == object_states_.end()) {
          return core::Result<core::Pose3DTO>::Err(
              core::ErrorDTO{core::kErrorMissingReference,
                             "Object reference not found", details});
        }

        Eigen::Matrix4d t_frame_object = pose_to_matrix(it->second.pose);
        Eigen::Matrix4d t_world_frame =
            frame_to_world_matrix_locked(it->second.frame_id);
        Eigen::Matrix4d t_world_object = t_world_frame * t_frame_object;
        Eigen::Matrix4d t_world_base = frame_to_world_matrix_locked(base_frame);
        return core::Result<core::Pose3DTO>::Ok(
            matrix_to_pose(t_world_base.inverse() * t_world_object));
      }

      case core::ReferenceDTO::Type::kFrame:
      case core::ReferenceDTO::Type::kRobotLink: {
        details["reference_type"] =
            reference.type == core::ReferenceDTO::Type::kFrame ? "frame" : "robot_link";
        if (reference.id.empty()) {
          return core::Result<core::Pose3DTO>::Err(
              core::ErrorDTO{core::kErrorMissingReference,
                             "Reference id is empty", details});
        }
        try {
          Eigen::Matrix4d t_world_ref = frame_to_world_matrix_locked(reference.id);
          Eigen::Matrix4d t_world_base = frame_to_world_matrix_locked(base_frame);
          return core::Result<core::Pose3DTO>::Ok(
              matrix_to_pose(t_world_base.inverse() * t_world_ref));
        } catch (const std::exception&) {
          return core::Result<core::Pose3DTO>::Err(
              core::ErrorDTO{core::kErrorMissingReference,
                             "Reference frame not found", details});
        }
      }

      case core::ReferenceDTO::Type::kPose: {
        details["reference_type"] = "pose";
        if (!reference.pose.has_value()) {
          return core::Result<core::Pose3DTO>::Err(
              core::ErrorDTO{core::kErrorMissingReference,
                             "Pose reference is missing inline pose data", details});
        }
        return core::Result<core::Pose3DTO>::Ok(reference.pose.value());
      }
    }
  } catch (const std::exception& e) {
    return core::Result<core::Pose3DTO>::Err(
        core::ErrorDTO{core::kErrorInternalError, e.what(), {}});
  }

  return core::Result<core::Pose3DTO>::Err(
      core::ErrorDTO{core::kErrorInvalidRequest,
                     "Unsupported reference type", {}});
}

core::JointLimitsDTO RoboPlanPlanningContext::get_joint_limits(
    const std::string& /*robot_id*/, const std::string& /*group_id*/) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  core::JointLimitsDTO limits;
  const auto maybe_group_info = scene_->getJointGroupInfo(group_name_);
  if (!maybe_group_info) {
    throw std::runtime_error("Failed to get joint limits: " + maybe_group_info.error());
  }
  limits.joint_names = maybe_group_info->joint_names;

  auto pos_limits = scene_->getPositionLimitVectors(group_name_, /*collapsed=*/false);
  if (pos_limits.has_value()) {
    const auto& [lower, upper] = pos_limits.value();
    limits.position_lower_rad.resize(lower.size());
    limits.position_upper_rad.resize(upper.size());
    for (Eigen::Index i = 0; i < lower.size(); ++i) {
      limits.position_lower_rad[i] = lower[i];
      limits.position_upper_rad[i] = upper[i];
    }
  }

  auto vel_limits = scene_->getVelocityLimitVectors(group_name_);
  if (vel_limits.has_value()) {
    const auto& [lower, upper] = vel_limits.value();
    limits.velocity_max_rad_s.resize(upper.size());
    for (Eigen::Index i = 0; i < upper.size(); ++i) {
      limits.velocity_max_rad_s[i] = upper[i];
    }
  }

  auto acc_limits = scene_->getAccelerationLimitVectors(group_name_);
  if (acc_limits.has_value()) {
    const auto& [lower, upper] = acc_limits.value();
    limits.acceleration_max_rad_s2.resize(upper.size());
    for (Eigen::Index i = 0; i < upper.size(); ++i) {
      limits.acceleration_max_rad_s2[i] = upper[i];
    }
  }

  return limits;
}

std::vector<std::string> RoboPlanPlanningContext::get_scene_object_ids() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  std::vector<std::string> object_ids;
  object_ids.reserve(object_states_.size());
  for (const auto& [object_id, tracked] : object_states_) {
    static_cast<void>(tracked);
    object_ids.push_back(object_id);
  }
  return object_ids;
}

std::vector<std::string> RoboPlanPlanningContext::get_joint_names() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  const auto maybe_group_info = scene_->getJointGroupInfo(group_name_);
  if (!maybe_group_info) {
    throw std::runtime_error("Failed to get joint names: " + maybe_group_info.error());
  }
  return maybe_group_info->joint_names;
}

std::vector<RoboPlanPlanningContext::SceneObjectSnapshot>
RoboPlanPlanningContext::get_scene_objects() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  return scene_objects_locked();
}

std::vector<RoboPlanPlanningContext::SceneObjectSnapshot>
RoboPlanPlanningContext::scene_objects_locked() {
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
        tracked.support_contacts,
    });
  }
  return objects;
}

std::vector<std::pair<std::string, core::CollisionBodyId>>
RoboPlanPlanningContext::robot_body_mappings_locked(
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

std::vector<std::string> RoboPlanPlanningContext::object_body_ids_for_state_locked(
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
RoboPlanPlanningContext::object_body_ids_for_transition_locked(
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
RoboPlanPlanningContext::robot_snapshot_bodies_locked(
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
RoboPlanPlanningContext::geometry_descriptor_for_shape_locked(
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
RoboPlanPlanningContext::object_snapshot_bodies_locked(
    const std::string& robot_id) const {
  std::vector<core::CollisionWorldBody> bodies;
  bodies.reserve(object_states_.size());

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
    body.source_timestamp_ns = tracked.source_timestamp_ns;
    body.source_freshness_budget_ns = tracked.source_freshness_budget_ns;
    body.geometries.push_back(geometry_descriptor_for_shape_locked(
        body.body_id, body.owner_kind, tracked.shape));
    bodies.push_back(std::move(body));
  }

  return bodies;
}

std::string RoboPlanPlanningContext::robot_model_frame_id_locked() const {
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

Eigen::Matrix4d RoboPlanPlanningContext::robot_model_frame_world_matrix_locked(
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
RoboPlanPlanningContext::build_collision_world_snapshot_locked() const {
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
RoboPlanPlanningContext::collision_policy_snapshot() const {
  std::string robot_id;
  {
    std::scoped_lock map_lock(mapper_mutex_);
    robot_id = mapper_robot_id_;
  }

  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  return build_collision_policy_snapshot_locked(robot_id);
}

core::CollisionPolicySnapshot
RoboPlanPlanningContext::build_collision_policy_snapshot_locked(
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
      .mix(world_revision_id_)
      .mix(world_content_hash_)
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
  policy.revision.revision_id = world_revision_id_;
  policy.revision.content_hash = h.finalize();
  return policy;
}

std::string RoboPlanPlanningContext::compute_collision_world_content_hash_locked(
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

Eigen::Matrix4d RoboPlanPlanningContext::pose_to_matrix(
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

core::Pose3DTO RoboPlanPlanningContext::matrix_to_pose(
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

bool RoboPlanPlanningContext::is_world_frame(const std::string& frame_id) const {
  return frame_id.empty() || frame_id == "universe" || frame_id == "world" ||
         frame_id == "World" || frame_id == "/World";
}

Eigen::Matrix4d RoboPlanPlanningContext::scene_placement_from_tracked_locked(
    const TrackedObjectState& state) const {
  const Eigen::Matrix4d t_frame_object = pose_to_matrix(state.pose);
  if (is_world_frame(state.frame_id)) {
    return world_from_robot_base_.inverse() * t_frame_object;
  }
  return parent_joint_from_frame_matrix_locked(state.frame_id) * t_frame_object;
}

Eigen::Matrix4d RoboPlanPlanningContext::parent_joint_from_frame_matrix_locked(
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

Eigen::Matrix4d RoboPlanPlanningContext::frame_to_world_matrix_locked(
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

core::Pose3DTO RoboPlanPlanningContext::pose_in_base_from_tracked_locked(
    const TrackedObjectState& state, const std::string& base_frame) const {
  const Eigen::Matrix4d t_frame_object = pose_to_matrix(state.pose);
  const Eigen::Matrix4d t_world_frame = frame_to_world_matrix_locked(state.frame_id);
  const Eigen::Matrix4d t_world_object = t_world_frame * t_frame_object;
  const Eigen::Matrix4d t_world_base = frame_to_world_matrix_locked(base_frame);
  return matrix_to_pose(t_world_base.inverse() * t_world_object);
}

bool RoboPlanPlanningContext::infer_attached_from_frame(
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

std::vector<std::string> RoboPlanPlanningContext::normalize_touch_links_locked(
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

tl::expected<void, std::string> RoboPlanPlanningContext::set_touch_links_locked(
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

tl::expected<void, std::string> RoboPlanPlanningContext::clear_touch_links_locked(
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
std::pair<std::string, std::string> RoboPlanPlanningContext::canonical_collision_pair(
    const std::string& body1,
    const std::string& body2) const {
  if (body1 < body2) {
    return {body1, body2};
  }
  return {body2, body1};
}

std::string RoboPlanPlanningContext::collision_pair_text(
    const std::pair<std::string, std::string>& pair) const {
  return pair.first + " <-> " + pair.second;
}

std::optional<std::pair<std::string, std::string>>
RoboPlanPlanningContext::canonical_object_collision_pair_locked(
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

RoboPlanPlanningContext::CollisionDiagnostics
RoboPlanPlanningContext::collect_collision_diagnostics_locked(
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

void RoboPlanPlanningContext::add_collision_details(
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
RoboPlanPlanningContext::apply_collision_pair_projection_locked(
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
RoboPlanPlanningContext::apply_static_collision_policy_for_object_locked(
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

void RoboPlanPlanningContext::erase_static_collision_policy_for_object_locked(
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
RoboPlanPlanningContext::recompute_support_contact_policy_locked() {
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
RoboPlanPlanningContext::set_support_contact_pairs_for_validation_locked(
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
RoboPlanPlanningContext::active_support_contact_pairs_for_validation_locked() const {
  return disabled_support_contact_pairs_;
}

bool RoboPlanPlanningContext::is_active_support_contact_pair_locked(
    const std::pair<std::string, std::string>& pair) const {
  return is_support_contact_pair_locked(pair);
}

bool RoboPlanPlanningContext::is_support_contact_pair_locked(
    const std::pair<std::string, std::string>& pair) const {
  return disabled_support_contact_pairs_.find(pair) !=
         disabled_support_contact_pairs_.end();
}

std::optional<core::SupportContactDTO>
RoboPlanPlanningContext::support_contact_policy_for_pair_locked(
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

void RoboPlanPlanningContext::record_scene_event_locked(SceneTraceEvent event) {
  if (event.timestamp_ns == 0) {
    event.timestamp_ns = steady_now_ns();
  }
  scene_trace_.push_back(std::move(event));
  constexpr size_t kMaxSceneTraceEvents = 4096;
  while (scene_trace_.size() > kMaxSceneTraceEvents) {
    scene_trace_.pop_front();
  }
}

tl::expected<void, std::string> RoboPlanPlanningContext::add_geometry_locked(
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

core::SceneEditResultDTO RoboPlanPlanningContext::reparent_object_locked(
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

core::SceneEditResultDTO RoboPlanPlanningContext::add_object(
    const core::AddObjectRequestDTO& request) {
  core::SceneEditResultDTO result;

  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);

    const std::string final_frame_id =
        is_world_frame(request.frame_id) ? "universe" : request.frame_id;
    const bool attached = infer_attached_from_frame(final_frame_id);

    TrackedObjectState new_state{
        request.pose,
        request.shape,
        final_frame_id,
        attached,
        {},
        request.support_contacts,
        {},
        {},
    };

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

core::SceneEditResultDTO RoboPlanPlanningContext::remove_object(
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

core::SceneEditResultDTO RoboPlanPlanningContext::update_object_pose(
    const core::UpdateObjectPoseRequestDTO& request) {
  core::SceneEditResultDTO result;

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
    new_state.source_timestamp_ns = request.source_timestamp_ns;
    new_state.source_freshness_budget_ns = request.source_freshness_budget_ns;

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
    publish_collision_world_revision_locked(
        core::CollisionWorldMutationSource::kSceneEdit,
        object_body_ids_for_transition_locked(
            request.object_id, previous_state, tracked),
        "scene_edit.update_object_pose");
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

core::SceneEditResultDTO RoboPlanPlanningContext::attach_object(
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

core::SceneEditResultDTO RoboPlanPlanningContext::detach_object(
    const core::DetachObjectRequestDTO& request) {
  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    const std::string target_frame = request.frame_id.empty() ? "universe" : request.frame_id;
    const auto it = object_states_.find(request.object_id);
    if (it == object_states_.end()) {
      return core::SceneEditResultDTO{
          false,
          core::ErrorDTO{core::kErrorObjectNotFound,
                         "Object not found: " + request.object_id, {}},
      };
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

std::vector<RoboPlanPlanningContext::SceneTraceEvent>
RoboPlanPlanningContext::get_scene_trace(size_t limit) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  if (limit == 0 || limit >= scene_trace_.size()) {
    return {scene_trace_.begin(), scene_trace_.end()};
  }
  return {scene_trace_.end() - static_cast<std::ptrdiff_t>(limit), scene_trace_.end()};
}

void RoboPlanPlanningContext::clear_scene_trace() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  scene_trace_.clear();
}

}  // namespace manyforge::roboplan_backend
