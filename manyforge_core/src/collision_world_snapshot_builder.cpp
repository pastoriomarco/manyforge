#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <manyforge_core/types/content_hash.hpp>

namespace manyforge::core {

namespace {

std::vector<std::string> split_body_id(const std::string& value) {
  std::vector<std::string> parts;
  std::size_t cursor = 0;
  while (cursor <= value.size()) {
    const std::size_t slash = value.find('/', cursor);
    const std::size_t end = slash == std::string::npos ? value.size() : slash;
    parts.emplace_back(value.substr(cursor, end - cursor));
    if (slash == std::string::npos) {
      break;
    }
    cursor = slash + 1;
  }
  return parts;
}

bool all_non_empty(const std::vector<std::string>& parts) {
  return std::all_of(parts.begin(), parts.end(),
                     [](const std::string& part) { return !part.empty(); });
}

bool body_id_matches_owner_kind(const CollisionWorldBody& body) {
  const auto parts = split_body_id(body.body_id);
  switch (body.owner_kind) {
    case CollisionOwnerKind::kRobotLink:
      return parts.size() == 6 && all_non_empty(parts) &&
             parts[0] == "robot" && parts[2] == "link" &&
             parts[4] == "collision";
    case CollisionOwnerKind::kAttachedObject:
      return parts.size() == 6 && all_non_empty(parts) &&
             parts[0] == "robot" && parts[2] == "attached" &&
             parts[4] == "shape";
    case CollisionOwnerKind::kDetachedObject:
      return parts.size() == 4 && all_non_empty(parts) &&
             parts[0] == "object" && parts[2] == "shape";
    case CollisionOwnerKind::kFixture:
      return parts.size() == 4 && all_non_empty(parts) &&
             parts[0] == "fixture" && parts[2] == "shape";
  }
  return false;
}

bool parent_link_has_robot_link_shape(const std::string& parent_link) {
  const auto parts = split_body_id(parent_link);
  return parts.size() == 4 && all_non_empty(parts) && parts[0] == "robot" &&
         parts[2] == "link";
}

void mix_pose(ContentHasher& hasher, const Pose3DTO& pose) {
  hasher.mix(pose.position_m.x_m);
  hasher.mix(pose.position_m.y_m);
  hasher.mix(pose.position_m.z_m);
  hasher.mix(pose.orientation.x);
  hasher.mix(pose.orientation.y);
  hasher.mix(pose.orientation.z);
  hasher.mix(pose.orientation.w);
}

void mix_vector3(ContentHasher& hasher, const Vector3DTO& vector) {
  hasher.mix(vector.x_m);
  hasher.mix(vector.y_m);
  hasher.mix(vector.z_m);
}

bool finite_pose(const Pose3DTO& pose) {
  return std::isfinite(pose.position_m.x_m) &&
         std::isfinite(pose.position_m.y_m) &&
         std::isfinite(pose.position_m.z_m) &&
         std::isfinite(pose.orientation.x) &&
         std::isfinite(pose.orientation.y) &&
         std::isfinite(pose.orientation.z) &&
         std::isfinite(pose.orientation.w);
}

bool positive_finite(double value) {
  return std::isfinite(value) && value > 0.0;
}

bool valid_positive_dimensions(const Vector3DTO& dimensions) {
  return positive_finite(dimensions.x_m) && positive_finite(dimensions.y_m) &&
         positive_finite(dimensions.z_m);
}

bool valid_mesh_scale(const Vector3DTO& scale) {
  return positive_finite(scale.x_m) && positive_finite(scale.y_m) &&
         positive_finite(scale.z_m);
}

bool has_unrelated_dimensions(const CollisionGeometryDescriptor& geom,
                              bool allow_box,
                              bool allow_sphere,
                              bool allow_cylinder,
                              bool allow_capsule) {
  if (!allow_box && geom.box_dimensions_m.has_value()) {
    return true;
  }
  if (!allow_sphere && geom.sphere_radius_m.has_value()) {
    return true;
  }
  if (!allow_cylinder &&
      (geom.cylinder_radius_m.has_value() ||
       geom.cylinder_height_m.has_value())) {
    return true;
  }
  if (!allow_capsule &&
      (geom.capsule_radius_m.has_value() || geom.capsule_height_m.has_value())) {
    return true;
  }
  return false;
}

std::optional<std::string> validate_geometry_shape_fields(
    const CollisionGeometryDescriptor& geom) {
  const bool mesh_like =
      geom.geometry_kind == CollisionGeometryKind::kConvexMesh ||
      geom.geometry_kind == CollisionGeometryKind::kTriangleMesh ||
      geom.geometry_kind == CollisionGeometryKind::kCompound;
  if (!mesh_like && geom.mesh_scale.has_value()) {
    return "primitive geometry carries mesh_scale";
  }
  switch (geom.geometry_kind) {
    case CollisionGeometryKind::kBox:
      if (has_unrelated_dimensions(geom, true, false, false, false)) {
        return "box geometry carries unrelated primitive dimensions";
      }
      if (!geom.box_dimensions_m.has_value() ||
          !valid_positive_dimensions(*geom.box_dimensions_m)) {
        return "box geometry requires positive finite box_dimensions_m";
      }
      return std::nullopt;
    case CollisionGeometryKind::kSphere:
      if (has_unrelated_dimensions(geom, false, true, false, false)) {
        return "sphere geometry carries unrelated primitive dimensions";
      }
      if (!geom.sphere_radius_m.has_value() ||
          !positive_finite(*geom.sphere_radius_m)) {
        return "sphere geometry requires positive finite sphere_radius_m";
      }
      return std::nullopt;
    case CollisionGeometryKind::kCylinder:
      if (has_unrelated_dimensions(geom, false, false, true, false)) {
        return "cylinder geometry carries unrelated primitive dimensions";
      }
      if (!geom.cylinder_radius_m.has_value() ||
          !positive_finite(*geom.cylinder_radius_m) ||
          !geom.cylinder_height_m.has_value() ||
          !positive_finite(*geom.cylinder_height_m)) {
        return "cylinder geometry requires positive finite radius and height";
      }
      return std::nullopt;
    case CollisionGeometryKind::kCapsule:
      if (has_unrelated_dimensions(geom, false, false, false, true)) {
        return "capsule geometry carries unrelated primitive dimensions";
      }
      if (!geom.capsule_radius_m.has_value() ||
          !positive_finite(*geom.capsule_radius_m) ||
          !geom.capsule_height_m.has_value() ||
          !positive_finite(*geom.capsule_height_m)) {
        return "capsule geometry requires positive finite radius and height";
      }
      return std::nullopt;
    case CollisionGeometryKind::kConvexMesh:
    case CollisionGeometryKind::kTriangleMesh:
    case CollisionGeometryKind::kCompound:
      if (has_unrelated_dimensions(geom, false, false, false, false)) {
        return "mesh/compound geometry carries primitive dimensions";
      }
      if (geom.mesh_scale.has_value() && !valid_mesh_scale(*geom.mesh_scale)) {
        return "mesh/compound geometry requires positive finite mesh_scale";
      }
      if ((!geom.source_uri.has_value() || geom.source_uri->empty()) &&
          (!geom.source_hash.has_value() || geom.source_hash->empty())) {
        return "mesh/compound geometry requires source_uri or source_hash";
      }
      return std::nullopt;
  }
  return "unknown geometry kind";
}

void mix_geometry(ContentHasher& hasher,
                  const CollisionGeometryDescriptor& geom) {
  hasher.mix(geom.body_id);
  hasher.mix(static_cast<std::int64_t>(geom.owner_kind));
  hasher.mix(static_cast<std::int64_t>(geom.geometry_kind));
  mix_pose(hasher, geom.local_pose);
  hasher.mix(static_cast<std::int64_t>(geom.box_dimensions_m.has_value() ? 1 : 0));
  if (geom.box_dimensions_m) {
    mix_vector3(hasher, *geom.box_dimensions_m);
  }
  hasher.mix(geom.sphere_radius_m.value_or(-1.0));
  hasher.mix(geom.cylinder_radius_m.value_or(-1.0));
  hasher.mix(geom.cylinder_height_m.value_or(-1.0));
  hasher.mix(geom.capsule_radius_m.value_or(-1.0));
  hasher.mix(geom.capsule_height_m.value_or(-1.0));
  hasher.mix(static_cast<std::int64_t>(geom.mesh_scale.has_value() ? 1 : 0));
  if (geom.mesh_scale) {
    mix_vector3(hasher, *geom.mesh_scale);
  }
  hasher.mix(geom.source_uri.value_or(""));
  hasher.mix(geom.source_hash.value_or(""));
  // Quality labels are sorted so a publisher emitting {"convex","watertight"}
  // and {"watertight","convex"} produces the same digest.
  std::vector<std::string> labels = geom.quality_labels;
  std::sort(labels.begin(), labels.end());
  hasher.mix(static_cast<std::int64_t>(labels.size()));
  for (const auto& label : labels) {
    hasher.mix(label);
  }
  hasher.mix(geom.triangle_count.value_or(-1));
  hasher.mix(geom.convex_part_count.value_or(-1));
  hasher.mix(geom.memory_estimate_bytes.value_or(-1));
}

std::string compute_body_content_hash(const CollisionWorldBody& body) {
  ContentHasher h;
  h.mix(body.body_id);
  h.mix(static_cast<std::int64_t>(body.owner_kind));
  h.mix(body.parent_link.value_or(""));
  h.mix(body.frame_id);
  mix_pose(h, body.pose);
  h.mix(static_cast<std::int64_t>(body.geometries.size()));
  for (const auto& geom : body.geometries) {
    mix_geometry(h, geom);
  }
  return h.finalize();
}

Result<CollisionWorldSnapshot> err(std::string detail) {
  ErrorDTO e;
  e.code = std::string(to_string(ErrorCode::kInvariantViolation));
  e.message = std::move(detail);
  return Result<CollisionWorldSnapshot>::Err(std::move(e));
}

}  // namespace

CollisionWorldSnapshotBuilder& CollisionWorldSnapshotBuilder::set_publisher_id(
    std::string id) {
  publisher_id_ = std::move(id);
  return *this;
}

CollisionWorldSnapshotBuilder&
CollisionWorldSnapshotBuilder::set_snapshot_format_version(
    std::uint32_t version) {
  snapshot_format_version_ = version;
  return *this;
}

CollisionWorldSnapshotBuilder& CollisionWorldSnapshotBuilder::set_revision_id(
    std::uint64_t revision_id) {
  revision_id_ = revision_id;
  return *this;
}

CollisionWorldSnapshotBuilder& CollisionWorldSnapshotBuilder::set_emitted_at_ns(
    std::int64_t emitted_at_ns) {
  emitted_at_ns_ = emitted_at_ns;
  return *this;
}

CollisionWorldSnapshotBuilder&
CollisionWorldSnapshotBuilder::add_robot_model_frame(
    CollisionRobotModelFrame frame) {
  robot_model_frames_.push_back(std::move(frame));
  return *this;
}

CollisionWorldSnapshotBuilder& CollisionWorldSnapshotBuilder::add_body(
    CollisionWorldBody body) {
  bodies_.push_back(std::move(body));
  return *this;
}

Result<CollisionWorldSnapshot> CollisionWorldSnapshotBuilder::build() {
  if (publisher_id_.empty()) {
    return err("publisher_id must be non-empty");
  }
  if (revision_id_ == 0) {
    return err("revision_id must be non-zero (0 is reserved for unset)");
  }
  if (snapshot_format_version_ == 0) {
    return err("snapshot_format_version must be non-zero");
  }
  if (emitted_at_ns_ < 0) {
    return err("emitted_at_ns must be non-negative");
  }

  CollisionWorldSnapshot snapshot;
  snapshot.snapshot_format_version = snapshot_format_version_;
  snapshot.publisher_id = publisher_id_;
  snapshot.revision.revision_id = revision_id_;
  snapshot.emitted_at_ns = emitted_at_ns_;
  snapshot.robot_model_frames = std::move(robot_model_frames_);
  robot_model_frames_.clear();
  snapshot.bodies = std::move(bodies_);
  bodies_.clear();

  std::set<std::pair<std::string, std::string>> seen_robot_frames;
  for (const auto& frame : snapshot.robot_model_frames) {
    if (frame.robot_id.empty()) {
      return err("robot_model_frame robot_id must be non-empty");
    }
    if (frame.model_frame_id.empty()) {
      return err("robot_model_frame model_frame_id must be non-empty");
    }
    if (!finite_pose(frame.world_pose)) {
      return err("robot_model_frame world_pose must be finite");
    }
    const auto key = std::make_pair(frame.robot_id, frame.model_frame_id);
    if (!seen_robot_frames.insert(key).second) {
      return err("duplicate robot_model_frame: " + frame.robot_id + "/" +
                 frame.model_frame_id);
    }
  }

  std::sort(snapshot.robot_model_frames.begin(),
            snapshot.robot_model_frames.end(),
            [](const CollisionRobotModelFrame& a,
               const CollisionRobotModelFrame& b) {
              if (a.robot_id != b.robot_id) {
                return a.robot_id < b.robot_id;
              }
              return a.model_frame_id < b.model_frame_id;
            });

  std::set<CollisionBodyId> seen_ids;
  for (const auto& body : snapshot.bodies) {
    if (body.body_id.empty()) {
      return err("body_id must be non-empty");
    }
    if (!body_id_matches_owner_kind(body)) {
      std::ostringstream oss;
      oss << "body_id does not match owner_kind: " << body.body_id;
      return err(oss.str());
    }
    if (body.frame_id.empty()) {
      std::ostringstream oss;
      oss << "frame_id must be non-empty for body: " << body.body_id;
      return err(oss.str());
    }
    if (!seen_ids.insert(body.body_id).second) {
      std::ostringstream oss;
      oss << "duplicate body_id: " << body.body_id;
      return err(oss.str());
    }
    if (body.geometries.empty()) {
      std::ostringstream oss;
      oss << "body has no geometries: " << body.body_id;
      return err(oss.str());
    }
    for (const auto& geom : body.geometries) {
      if (geom.body_id != body.body_id) {
        std::ostringstream oss;
        oss << "geometry body_id mismatch: body=" << body.body_id
            << " geometry=" << geom.body_id;
        return err(oss.str());
      }
      if (geom.owner_kind != body.owner_kind) {
        std::ostringstream oss;
        oss << "geometry owner_kind mismatch: body=" << body.body_id;
        return err(oss.str());
      }
      if (auto shape_error = validate_geometry_shape_fields(geom);
          shape_error.has_value()) {
        std::ostringstream oss;
        oss << *shape_error << " body=" << body.body_id;
        return err(oss.str());
      }
    }
    const bool is_attached =
        body.owner_kind == CollisionOwnerKind::kAttachedObject;
    if (is_attached) {
      if (!body.parent_link.has_value() || body.parent_link->empty()) {
        std::ostringstream oss;
        oss << "attached body missing parent_link: " << body.body_id;
        return err(oss.str());
      }
      if (!parent_link_has_robot_link_shape(*body.parent_link)) {
        std::ostringstream oss;
        oss << "attached body parent_link is not a robot link id: "
            << body.body_id;
        return err(oss.str());
      }
    } else if (body.parent_link.has_value()) {
      std::ostringstream oss;
      oss << "non-attached body has parent_link: " << body.body_id;
      return err(oss.str());
    }
  }

  // Sort bodies deterministically.
  std::sort(snapshot.bodies.begin(), snapshot.bodies.end(),
            [](const CollisionWorldBody& a, const CollisionWorldBody& b) {
              return a.body_id < b.body_id;
            });

  // Populate counts + per-body hashes + flags.
  snapshot.body_count = static_cast<std::int64_t>(snapshot.bodies.size());
  snapshot.geometry_count = 0;
  snapshot.robot_link_count = 0;
  snapshot.attached_object_count = 0;
  snapshot.detached_object_count = 0;
  snapshot.fixture_count = 0;
  snapshot.contains_mesh_geometry = false;
  snapshot.all_bodies_have_geometry = true;

  for (auto& body : snapshot.bodies) {
    body.body_content_hash = compute_body_content_hash(body);
    snapshot.geometry_count +=
        static_cast<std::int64_t>(body.geometries.size());
    switch (body.owner_kind) {
      case CollisionOwnerKind::kRobotLink:
        snapshot.robot_link_count++;
        break;
      case CollisionOwnerKind::kAttachedObject:
        snapshot.attached_object_count++;
        break;
      case CollisionOwnerKind::kDetachedObject:
        snapshot.detached_object_count++;
        break;
      case CollisionOwnerKind::kFixture:
        snapshot.fixture_count++;
        break;
    }
    for (const auto& geom : body.geometries) {
      if (geom.geometry_kind == CollisionGeometryKind::kTriangleMesh ||
          geom.geometry_kind == CollisionGeometryKind::kCompound) {
        snapshot.contains_mesh_geometry = true;
      }
    }
    if (body.geometries.empty()) {
      snapshot.all_bodies_have_geometry = false;
    }
  }

  // Compose the snapshot-level content hash.
  ContentHasher world;
  world.mix("manyforge_core/CollisionWorldSnapshot");
  world.mix(static_cast<std::int64_t>(snapshot.snapshot_format_version));
  world.mix(snapshot.publisher_id);
  world.mix(static_cast<std::int64_t>(snapshot.robot_model_frames.size()));
  for (const auto& frame : snapshot.robot_model_frames) {
    world.mix(frame.robot_id);
    world.mix(frame.model_frame_id);
    mix_pose(world, frame.world_pose);
  }
  world.mix(static_cast<std::int64_t>(snapshot.bodies.size()));
  for (const auto& body : snapshot.bodies) {
    world.mix(body.body_content_hash);
  }
  snapshot.revision.content_hash = world.finalize();

  return Result<CollisionWorldSnapshot>::Ok(std::move(snapshot));
}

Result<CollisionWorldSnapshotPtr>
CollisionWorldSnapshotBuilder::build_shared() {
  auto result = build();
  if (!result.ok()) {
    return Result<CollisionWorldSnapshotPtr>::Err(result.error());
  }
  auto ptr =
      std::make_shared<const CollisionWorldSnapshot>(std::move(result.value()));
  return Result<CollisionWorldSnapshotPtr>::Ok(std::move(ptr));
}

}  // namespace manyforge::core
