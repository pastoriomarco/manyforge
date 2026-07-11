#include <manyforge_roboplan/collision_body_id_mapper.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace manyforge::roboplan_backend {

namespace {

void fnv1a_mix(std::uint64_t& h, const std::string& s) {
  for (const unsigned char c : s) {
    h ^= static_cast<std::uint64_t>(c);
    h *= 1099511628211ULL;
  }
  // Delimiter prevents ["ab", "c"] and ["a", "bc"] from collapsing into the
  // same byte stream.
  h ^= 0xffU;
  h *= 1099511628211ULL;
}

/// Deterministic content hash for the mapping. Stable for identical input
/// regardless of std::unordered_map iteration order: input pairs are sorted
/// before hashing. Hex-encoded for human-readable diagnostics. This is not a
/// cryptographic hash; it is a process-independent diagnostic fingerprint for
/// Chunk 1 until the Chunk 2 collision-world ledger lands.
std::string compute_content_hash(
    const std::string& robot_id,
    const std::vector<std::pair<std::string, core::CollisionBodyId>>&
        sorted_pairs) {
  std::uint64_t h = 1469598103934665603ULL;
  fnv1a_mix(h, robot_id);
  for (const auto& [raw, body_id] : sorted_pairs) {
    fnv1a_mix(h, raw);
    fnv1a_mix(h, body_id);
  }

  std::ostringstream oss;
  oss << std::hex << h;
  return oss.str();
}

}  // namespace

RoboPlanCollisionBodyIdMapper::RoboPlanCollisionBodyIdMapper() = default;

void RoboPlanCollisionBodyIdMapper::rebuild(
    const std::string& robot_id,
    const std::vector<RoboPlanProjectionBackend::SceneObjectSnapshot>&
        scene_objects,
    const std::vector<std::pair<std::string, core::CollisionBodyId>>&
        robot_body_mappings) {
  raw_to_id_.clear();

  for (const auto& [raw_name, body_id] : robot_body_mappings) {
    raw_to_id_.emplace(raw_name, body_id);
  }

  // Scene objects: raw name in RoboPlan's Scene matches the ManyForge
  // object_id (RoboPlan stores authored objects by the id we passed in via
  // AddObjectRequestDTO::object_id). For attached objects, the body id uses
  // the robot/attached/... namespace; for detached, the object/... namespace.
  // shape_index is 0 for v0 single-shape objects; compound shapes use higher
  // indices in later chunks.
  constexpr int kShapeIndex = 0;
  for (const auto& obj : scene_objects) {
    core::CollisionBodyId body_id;
    if (obj.attached) {
      body_id = make_attached_object_id(robot_id, obj.object_id, kShapeIndex);
    } else {
      body_id = make_detached_object_id(obj.object_id, kShapeIndex);
    }
    raw_to_id_.emplace(obj.object_id, body_id);
  }

  // Compute content hash deterministically.
  std::vector<std::pair<std::string, core::CollisionBodyId>> sorted_pairs(
      raw_to_id_.begin(), raw_to_id_.end());
  std::sort(sorted_pairs.begin(), sorted_pairs.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  revision_.revision_id += 1;
  revision_.content_hash = compute_content_hash(robot_id, sorted_pairs);
}

std::optional<core::CollisionBodyId>
RoboPlanCollisionBodyIdMapper::lookup(const std::string& raw_name) const {
  auto it = raw_to_id_.find(raw_name);
  if (it == raw_to_id_.end()) {
    return std::nullopt;
  }
  return it->second;
}

core::CollisionWorldRevision
RoboPlanCollisionBodyIdMapper::current_revision() const {
  return revision_;
}

core::CollisionBodyId RoboPlanCollisionBodyIdMapper::make_robot_link_id(
    const std::string& robot_id,
    const std::string& link_name,
    int index) {
  std::ostringstream oss;
  oss << "robot/" << robot_id << "/link/" << link_name << "/collision/"
      << index;
  return oss.str();
}

core::CollisionBodyId RoboPlanCollisionBodyIdMapper::make_attached_object_id(
    const std::string& robot_id,
    const std::string& object_id,
    int shape_index) {
  std::ostringstream oss;
  oss << "robot/" << robot_id << "/attached/" << object_id << "/shape/"
      << shape_index;
  return oss.str();
}

core::CollisionBodyId RoboPlanCollisionBodyIdMapper::make_detached_object_id(
    const std::string& object_id,
    int shape_index) {
  std::ostringstream oss;
  oss << "object/" << object_id << "/shape/" << shape_index;
  return oss.str();
}

core::CollisionBodyId RoboPlanCollisionBodyIdMapper::make_fixture_id(
    const std::string& fixture_id,
    int shape_index) {
  std::ostringstream oss;
  oss << "fixture/" << fixture_id << "/shape/" << shape_index;
  return oss.str();
}

}  // namespace manyforge::roboplan_backend
