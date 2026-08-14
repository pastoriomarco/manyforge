#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

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

bool canonical_index(const std::string& value) {
  return !value.empty() && (value.size() == 1U || value.front() != '0') &&
         std::all_of(value.begin(), value.end(),
                     [](char digit) { return digit >= '0' && digit <= '9'; });
}

bool component_model_body_id(const std::string& value) {
  const auto parts = split_body_id(value);
  return parts.size() == 8U && all_non_empty(parts) && parts[0] == "component" &&
         parts[2] == "model" && parts[4] == "body" && parts[6] == "collision" &&
         canonical_index(parts[7]);
}

bool body_id_matches_owner_kind(const CollisionWorldBody& body) {
  const auto parts = split_body_id(body.body_id);
  switch (body.owner_kind) {
    case CollisionOwnerKind::kComponentModelBody:
      return component_model_body_id(body.body_id);
    case CollisionOwnerKind::kSceneResource:
      return parts.size() == 4U && all_non_empty(parts) && parts[0] == "scene" &&
             parts[2] == "shape" && canonical_index(parts[3]);
    case CollisionOwnerKind::kRuntimeObject:
      return parts.size() == 4U && all_non_empty(parts) && parts[0] == "runtime_object" &&
             parts[2] == "shape" && canonical_index(parts[3]);
  }
  return false;
}

bool finite_pose(const Pose3DTO& pose) {
  return std::isfinite(pose.position_m.x_m) && std::isfinite(pose.position_m.y_m) &&
         std::isfinite(pose.position_m.z_m) && std::isfinite(pose.orientation.x) &&
         std::isfinite(pose.orientation.y) && std::isfinite(pose.orientation.z) &&
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
  return positive_finite(scale.x_m) && positive_finite(scale.y_m) && positive_finite(scale.z_m);
}

bool has_unrelated_dimensions(const CollisionGeometryDescriptor& geom, bool allow_box,
                              bool allow_sphere, bool allow_cylinder, bool allow_capsule) {
  if (!allow_box && geom.box_dimensions_m.has_value()) {
    return true;
  }
  if (!allow_sphere && geom.sphere_radius_m.has_value()) {
    return true;
  }
  if (!allow_cylinder &&
      (geom.cylinder_radius_m.has_value() || geom.cylinder_height_m.has_value())) {
    return true;
  }
  if (!allow_capsule && (geom.capsule_radius_m.has_value() || geom.capsule_height_m.has_value())) {
    return true;
  }
  return false;
}

std::optional<std::string> validate_geometry_shape_fields(const CollisionGeometryDescriptor& geom) {
  const bool mesh_like = geom.geometry_kind == CollisionGeometryKind::kConvexMesh ||
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
      if (!geom.sphere_radius_m.has_value() || !positive_finite(*geom.sphere_radius_m)) {
        return "sphere geometry requires positive finite sphere_radius_m";
      }
      return std::nullopt;
    case CollisionGeometryKind::kCylinder:
      if (has_unrelated_dimensions(geom, false, false, true, false)) {
        return "cylinder geometry carries unrelated primitive dimensions";
      }
      if (!geom.cylinder_radius_m.has_value() || !positive_finite(*geom.cylinder_radius_m) ||
          !geom.cylinder_height_m.has_value() || !positive_finite(*geom.cylinder_height_m)) {
        return "cylinder geometry requires positive finite radius and height";
      }
      return std::nullopt;
    case CollisionGeometryKind::kCapsule:
      if (has_unrelated_dimensions(geom, false, false, false, true)) {
        return "capsule geometry carries unrelated primitive dimensions";
      }
      if (!geom.capsule_radius_m.has_value() || !positive_finite(*geom.capsule_radius_m) ||
          !geom.capsule_height_m.has_value() || !positive_finite(*geom.capsule_height_m)) {
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

using content_identity::CanonicalValue;

constexpr std::int64_t kMaximumSafeInteger = 9'007'199'254'740'991LL;

CanonicalValue number(const double value) {
  // RFC 8785 rejects negative zero. Collision geometry treats both zero
  // spellings identically, so normalize only that representation detail.
  return CanonicalValue(value == 0.0 ? 0.0 : value);
}

CanonicalValue safe_integer(const std::int64_t value) {
  if (value < -kMaximumSafeInteger || value > kMaximumSafeInteger) {
    throw content_identity::CanonicalValueError("integer exceeds the RFC 8785 safe range");
  }
  return CanonicalValue(static_cast<double>(value));
}

std::string_view owner_kind_token(const CollisionOwnerKind kind) {
  switch (kind) {
    case CollisionOwnerKind::kComponentModelBody:
      return "component_model_body";
    case CollisionOwnerKind::kSceneResource:
      return "scene_resource";
    case CollisionOwnerKind::kRuntimeObject:
      return "runtime_object";
  }
  throw content_identity::CanonicalValueError("unknown collision owner kind");
}

std::string_view geometry_kind_token(const CollisionGeometryKind kind) {
  switch (kind) {
    case CollisionGeometryKind::kBox:
      return "box";
    case CollisionGeometryKind::kSphere:
      return "sphere";
    case CollisionGeometryKind::kCylinder:
      return "cylinder";
    case CollisionGeometryKind::kCapsule:
      return "capsule";
    case CollisionGeometryKind::kConvexMesh:
      return "convex_mesh";
    case CollisionGeometryKind::kTriangleMesh:
      return "triangle_mesh";
    case CollisionGeometryKind::kCompound:
      return "compound";
  }
  throw content_identity::CanonicalValueError("unknown collision geometry kind");
}

CanonicalValue vector3(const Vector3DTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"x_m", number(value.x_m)},
      {"y_m", number(value.y_m)},
      {"z_m", number(value.z_m)},
  });
}

CanonicalValue pose(const Pose3DTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"position_m", vector3(value.position_m)},
      {"orientation", CanonicalValue(CanonicalValue::Object{
                          {"x", number(value.orientation.x)},
                          {"y", number(value.orientation.y)},
                          {"z", number(value.orientation.z)},
                          {"w", number(value.orientation.w)},
                      })},
  });
}

CanonicalValue geometry_content(const CollisionGeometryDescriptor& geometry) {
  CanonicalValue::Object result{
      {"body_id", CanonicalValue(geometry.body_id)},
      {"owner_kind", CanonicalValue(std::string(owner_kind_token(geometry.owner_kind)))},
      {"geometry_kind", CanonicalValue(std::string(geometry_kind_token(geometry.geometry_kind)))},
      {"local_pose", pose(geometry.local_pose)},
  };
  if (geometry.box_dimensions_m.has_value()) {
    result.emplace_back("box_dimensions_m", vector3(*geometry.box_dimensions_m));
  }
  if (geometry.sphere_radius_m.has_value()) {
    result.emplace_back("sphere_radius_m", number(*geometry.sphere_radius_m));
  }
  if (geometry.cylinder_radius_m.has_value()) {
    result.emplace_back("cylinder_radius_m", number(*geometry.cylinder_radius_m));
  }
  if (geometry.cylinder_height_m.has_value()) {
    result.emplace_back("cylinder_height_m", number(*geometry.cylinder_height_m));
  }
  if (geometry.capsule_radius_m.has_value()) {
    result.emplace_back("capsule_radius_m", number(*geometry.capsule_radius_m));
  }
  if (geometry.capsule_height_m.has_value()) {
    result.emplace_back("capsule_height_m", number(*geometry.capsule_height_m));
  }
  if (geometry.mesh_scale.has_value()) {
    result.emplace_back("mesh_scale", vector3(*geometry.mesh_scale));
  }
  if (geometry.source_uri.has_value()) {
    result.emplace_back("source_uri", CanonicalValue(*geometry.source_uri));
  }
  if (geometry.source_hash.has_value()) {
    result.emplace_back("source_hash", CanonicalValue(*geometry.source_hash));
  }

  std::vector<std::string> labels = geometry.quality_labels;
  std::sort(labels.begin(), labels.end());
  CanonicalValue::Array canonical_labels;
  canonical_labels.reserve(labels.size());
  for (const auto& label : labels) {
    canonical_labels.emplace_back(label);
  }
  result.emplace_back("quality_labels", CanonicalValue(std::move(canonical_labels)));

  if (geometry.triangle_count.has_value()) {
    result.emplace_back("triangle_count", safe_integer(*geometry.triangle_count));
  }
  if (geometry.convex_part_count.has_value()) {
    result.emplace_back("convex_part_count", safe_integer(*geometry.convex_part_count));
  }
  if (geometry.memory_estimate_bytes.has_value()) {
    result.emplace_back("memory_estimate_bytes", safe_integer(*geometry.memory_estimate_bytes));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue body_content(const CollisionWorldBody& body) {
  CanonicalValue::Object result{
      {"body_id", CanonicalValue(body.body_id)},
      {"owner_kind", CanonicalValue(std::string(owner_kind_token(body.owner_kind)))},
      {"frame_id", CanonicalValue(body.frame_id)},
      {"pose", pose(body.pose)},
  };
  if (body.parent_body_id.has_value()) {
    result.emplace_back("parent_body_id", CanonicalValue(*body.parent_body_id));
  }
  if (body.source_id.has_value()) {
    result.emplace_back("source_id", CanonicalValue(*body.source_id));
  }
  if (body.source_freshness_budget_ns.has_value()) {
    result.emplace_back("source_freshness_budget_ns",
                        safe_integer(*body.source_freshness_budget_ns));
  }
  CanonicalValue::Array geometries;
  geometries.reserve(body.geometries.size());
  for (const auto& geometry : body.geometries) {
    geometries.push_back(geometry_content(geometry));
  }
  result.emplace_back("geometries", CanonicalValue(std::move(geometries)));
  return CanonicalValue(std::move(result));
}

std::string compute_body_content_hash(const CollisionWorldBody& body) {
  return content_identity::framed_sha256(kCollisionWorldBodyContentIdentityDomain,
                                         kCollisionWorldBodyContentApiVersion, body_content(body));
}

CanonicalValue component_model_frame_content(const CollisionComponentModelFrame& frame) {
  return CanonicalValue(CanonicalValue::Object{
      {"controllable_component_id", CanonicalValue(frame.controllable_component_id.value())},
      {"model_instance_id", CanonicalValue(frame.model_instance_id.value())},
      {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
      {"compiled_frame_name", CanonicalValue(frame.compiled_frame_name)},
      {"model_revision", CanonicalValue(frame.model_revision.value())},
      {"world_pose", pose(frame.world_pose)},
  });
}

std::string compute_world_content_hash(const CollisionWorldSnapshot& snapshot,
                                       const std::string_view policy_overlay_content_hash) {
  CanonicalValue::Array frames;
  frames.reserve(snapshot.component_model_frames.size());
  for (const auto& frame : snapshot.component_model_frames) {
    frames.push_back(component_model_frame_content(frame));
  }

  CanonicalValue::Array bodies;
  bodies.reserve(snapshot.bodies.size());
  for (const auto& body : snapshot.bodies) {
    bodies.emplace_back(CanonicalValue::Object{
        {"body_id", CanonicalValue(body.body_id)},
        {"body_content_hash", CanonicalValue(body.body_content_hash)},
    });
  }

  CanonicalValue::Object payload{
      {"snapshot_format_version",
       CanonicalValue(static_cast<double>(snapshot.snapshot_format_version))},
      {"publisher_id", CanonicalValue(snapshot.publisher_id)},
      {"component_model_frames", CanonicalValue(std::move(frames))},
      {"bodies", CanonicalValue(std::move(bodies))},
  };
  if (!policy_overlay_content_hash.empty()) {
    payload.emplace_back("policy_overlay_content_hash",
                         CanonicalValue(std::string(policy_overlay_content_hash)));
  }
  return content_identity::framed_sha256(kCollisionWorldContentIdentityDomain,
                                         kCollisionWorldContentApiVersion,
                                         CanonicalValue(std::move(payload)));
}

Result<CollisionWorldSnapshot> err(std::string detail) {
  ErrorDTO e;
  e.code = std::string(to_string(ErrorCode::kInvariantViolation));
  e.message = std::move(detail);
  return Result<CollisionWorldSnapshot>::Err(std::move(e));
}

}  // namespace

std::string compute_collision_world_body_content_hash(const CollisionWorldBody& body) {
  return compute_body_content_hash(body);
}

CollisionWorldSnapshotBuilder& CollisionWorldSnapshotBuilder::set_publisher_id(std::string id) {
  publisher_id_ = std::move(id);
  return *this;
}

CollisionWorldSnapshotBuilder& CollisionWorldSnapshotBuilder::set_snapshot_format_version(
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

CollisionWorldSnapshotBuilder& CollisionWorldSnapshotBuilder::set_policy_overlay_content_hash(
    std::string content_hash) {
  policy_overlay_content_hash_ = std::move(content_hash);
  return *this;
}

CollisionWorldSnapshotBuilder& CollisionWorldSnapshotBuilder::add_component_model_frame(
    CollisionComponentModelFrame frame) {
  component_model_frames_.push_back(std::move(frame));
  return *this;
}

CollisionWorldSnapshotBuilder& CollisionWorldSnapshotBuilder::add_body(CollisionWorldBody body) {
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
  if (!policy_overlay_content_hash_.empty() &&
      !content_identity::is_sha256_digest(policy_overlay_content_hash_)) {
    return err("policy_overlay_content_hash must be exact lowercase sha256");
  }

  CollisionWorldSnapshot snapshot;
  snapshot.snapshot_format_version = snapshot_format_version_;
  snapshot.publisher_id = publisher_id_;
  snapshot.revision.revision_id = revision_id_;
  snapshot.emitted_at_ns = emitted_at_ns_;
  snapshot.component_model_frames = std::move(component_model_frames_);
  component_model_frames_.clear();
  snapshot.bodies = std::move(bodies_);
  bodies_.clear();

  using ComponentFrameKey = std::tuple<ControllableComponentId, ModelInstanceId, FrameRoleId>;
  std::set<ComponentFrameKey> seen_component_frames;
  for (const auto& frame : snapshot.component_model_frames) {
    if (frame.controllable_component_id.empty() || frame.model_instance_id.empty() ||
        frame.frame_role_id.empty() || frame.compiled_frame_name.empty() ||
        frame.model_revision.empty()) {
      return err("component_model_frame identity must be complete");
    }
    if (!finite_pose(frame.world_pose)) {
      return err("component_model_frame world_pose must be finite");
    }
    const ComponentFrameKey key{frame.controllable_component_id, frame.model_instance_id,
                                frame.frame_role_id};
    if (!seen_component_frames.insert(key).second) {
      return err("duplicate component_model_frame");
    }
  }

  std::sort(snapshot.component_model_frames.begin(), snapshot.component_model_frames.end(),
            [](const CollisionComponentModelFrame& a, const CollisionComponentModelFrame& b) {
              return std::tie(a.controllable_component_id, a.model_instance_id, a.frame_role_id) <
                     std::tie(b.controllable_component_id, b.model_instance_id, b.frame_role_id);
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
    if (body.source_timestamp_ns.has_value() && *body.source_timestamp_ns < 0) {
      std::ostringstream oss;
      oss << "source_timestamp_ns must be non-negative for body: " << body.body_id;
      return err(oss.str());
    }
    if (body.source_freshness_budget_ns.has_value()) {
      if (!body.source_timestamp_ns.has_value()) {
        std::ostringstream oss;
        oss << "source_freshness_budget_ns requires source_timestamp_ns for body: " << body.body_id;
        return err(oss.str());
      }
      if (*body.source_freshness_budget_ns <= 0) {
        std::ostringstream oss;
        oss << "source_freshness_budget_ns must be positive for body: " << body.body_id;
        return err(oss.str());
      }
      if (*body.source_freshness_budget_ns > kMaximumSafeInteger) {
        return err(
            "source_freshness_budget_ns exceeds the RFC 8785 safe "
            "range for body: " +
            body.body_id);
      }
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
        oss << "geometry body_id mismatch: body=" << body.body_id << " geometry=" << geom.body_id;
        return err(oss.str());
      }
      if (geom.owner_kind != body.owner_kind) {
        std::ostringstream oss;
        oss << "geometry owner_kind mismatch: body=" << body.body_id;
        return err(oss.str());
      }
      if (auto shape_error = validate_geometry_shape_fields(geom); shape_error.has_value()) {
        std::ostringstream oss;
        oss << *shape_error << " body=" << body.body_id;
        return err(oss.str());
      }
      const std::set<std::string> unique_labels(geom.quality_labels.begin(),
                                                geom.quality_labels.end());
      if (unique_labels.size() != geom.quality_labels.size() || unique_labels.contains("")) {
        return err(
            "geometry quality_labels must be nonempty and unique for "
            "body: " +
            body.body_id);
      }
      const auto valid_optional_count = [](const auto& value) {
        return !value.has_value() || (*value >= 0 && *value <= kMaximumSafeInteger);
      };
      if (!valid_optional_count(geom.triangle_count) ||
          !valid_optional_count(geom.convex_part_count) ||
          !valid_optional_count(geom.memory_estimate_bytes)) {
        return err(
            "geometry counts must be nonnegative RFC 8785 safe "
            "integers for body: " +
            body.body_id);
      }
    }
    if (body.parent_body_id.has_value() && (body.owner_kind != CollisionOwnerKind::kSceneResource ||
                                            !component_model_body_id(*body.parent_body_id))) {
      std::ostringstream oss;
      oss << "body has an invalid parent_body_id: " << body.body_id;
      return err(oss.str());
    }
  }

  for (const auto& body : snapshot.bodies) {
    if (!body.parent_body_id.has_value()) {
      continue;
    }
    const auto parent = std::find_if(
        snapshot.bodies.begin(), snapshot.bodies.end(), [&](const CollisionWorldBody& candidate) {
          return candidate.body_id == *body.parent_body_id &&
                 candidate.owner_kind == CollisionOwnerKind::kComponentModelBody;
        });
    if (parent == snapshot.bodies.end()) {
      return err("parent_body_id does not name a component-model body: " + body.body_id);
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
  snapshot.component_model_body_count = 0;
  snapshot.scene_resource_count = 0;
  snapshot.runtime_object_count = 0;
  snapshot.contains_mesh_geometry = false;
  snapshot.all_bodies_have_geometry = true;

  for (auto& body : snapshot.bodies) {
    try {
      body.body_content_hash = compute_collision_world_body_content_hash(body);
    } catch (const std::exception& exception) {
      return err("collision body content identity is invalid for body " + body.body_id + ": " +
                 exception.what());
    }
    snapshot.geometry_count += static_cast<std::int64_t>(body.geometries.size());
    switch (body.owner_kind) {
      case CollisionOwnerKind::kComponentModelBody:
        snapshot.component_model_body_count++;
        break;
      case CollisionOwnerKind::kSceneResource:
        snapshot.scene_resource_count++;
        break;
      case CollisionOwnerKind::kRuntimeObject:
        snapshot.runtime_object_count++;
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

  try {
    snapshot.revision.content_hash =
        compute_world_content_hash(snapshot, policy_overlay_content_hash_);
  } catch (const std::exception& exception) {
    return err("collision-world content identity is invalid: " + std::string(exception.what()));
  }

  return Result<CollisionWorldSnapshot>::Ok(std::move(snapshot));
}

Result<CollisionWorldSnapshotPtr> CollisionWorldSnapshotBuilder::build_shared() {
  auto result = build();
  if (!result.ok()) {
    return Result<CollisionWorldSnapshotPtr>::Err(result.error());
  }
  auto ptr = std::make_shared<const CollisionWorldSnapshot>(std::move(result.value()));
  return Result<CollisionWorldSnapshotPtr>::Ok(std::move(ptr));
}

}  // namespace manyforge::core
