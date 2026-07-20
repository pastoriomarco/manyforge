#include <manyforge_collision/manyforge_collision_engine.hpp>

#include <Eigen/Dense>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <sstream>
#include <utility>

#include <pinocchio/algorithm/geometry.hpp>
#include <pinocchio/algorithm/frames.hpp>
#include <pinocchio/algorithm/joint-configuration.hpp>
#include <pinocchio/algorithm/kinematics.hpp>
#include <pinocchio/collision/collision.hpp>
#include <pinocchio/collision/distance.hpp>
#include <pinocchio/parsers/srdf.hpp>
#include <pinocchio/parsers/urdf.hpp>

#include <coal/BVH/BVH_model.h>
#include <coal/mesh_loader/loader.h>
#include <coal/shape/geometric_shapes.h>

#ifndef MANYFORGE_COLLISION_ENGINE_VERSION
#define MANYFORGE_COLLISION_ENGINE_VERSION "0.1.0+unknown"
#endif

namespace manyforge::collision {

namespace {

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open file: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string regex_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size() * 2);
  for (char c : value) {
    switch (c) {
      case '\\':
      case '.':
      case '^':
      case '$':
      case '|':
      case '(':
      case ')':
      case '[':
      case ']':
      case '*':
      case '+':
      case '?':
      case '{':
      case '}':
        out.push_back('\\');
        [[fallthrough]];
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::optional<std::string> xml_attr(const std::string& text,
                                    const std::string& name) {
  const std::regex attr(name + R"(\s*=\s*["']([^"']+)["'])");
  std::smatch match;
  if (!std::regex_search(text, match, attr)) {
    return std::nullopt;
  }
  return match[1].str();
}

struct UrdfJoint {
  std::string name;
  std::string type;
  std::string parent_link;
  std::string child_link;
};

std::map<std::string, UrdfJoint> urdf_joints_by_child_link(
    const std::filesystem::path& urdf_path) {
  const std::string xml = read_text_file(urdf_path);
  const std::regex joint_re(R"(<joint\b([^>]*)>([\s\S]*?)</joint>)");
  std::map<std::string, UrdfJoint> out;
  for (auto it = std::sregex_iterator(xml.begin(), xml.end(), joint_re);
       it != std::sregex_iterator(); ++it) {
    const std::string attrs = (*it)[1].str();
    const std::string body = (*it)[2].str();
    auto name = xml_attr(attrs, "name");
    auto type = xml_attr(attrs, "type");
    auto parent = xml_attr(body, "parent\\s+link");
    auto child = xml_attr(body, "child\\s+link");
    if (!name || !type || !parent || !child) {
      continue;
    }
    out[*child] = UrdfJoint{*name, *type, *parent, *child};
  }
  return out;
}

std::vector<std::string> joints_for_chain_group(
    const std::filesystem::path& urdf_path,
    const std::string& base_link,
    const std::string& tip_link) {
  const auto by_child = urdf_joints_by_child_link(urdf_path);
  std::vector<std::string> reversed;
  std::string cursor = tip_link;
  std::set<std::string> seen;
  while (cursor != base_link) {
    if (!seen.insert(cursor).second) {
      throw std::runtime_error("cycle while resolving SRDF chain ending at " +
                               tip_link);
    }
    auto it = by_child.find(cursor);
    if (it == by_child.end()) {
      throw std::runtime_error("cannot resolve SRDF chain from " + base_link +
                               " to " + tip_link + ": missing parent joint for " +
                               cursor);
    }
    if (it->second.type != "fixed") {
      reversed.push_back(it->second.name);
    }
    cursor = it->second.parent_link;
  }
  std::reverse(reversed.begin(), reversed.end());
  return reversed;
}

std::vector<std::string> group_joint_names_from_srdf(
    const std::filesystem::path& srdf_path,
    const std::filesystem::path& urdf_path,
    const std::string& group_name) {
  if (group_name.empty() || srdf_path.empty() ||
      !std::filesystem::exists(srdf_path)) {
    return {};
  }
  const std::string xml = read_text_file(srdf_path);
  const std::regex group_re("<group\\b[^>]*name\\s*=\\s*[\"']" +
                            regex_escape(group_name) +
                            R"(["'][^>]*>([\s\S]*?)</group>)");
  std::smatch group_match;
  if (!std::regex_search(xml, group_match, group_re)) {
    return {};
  }
  const std::string body = group_match[1].str();

  std::vector<std::string> joints;
  const std::regex joint_re(R"(<joint\b([^>]*)/?>)");
  for (auto it = std::sregex_iterator(body.begin(), body.end(), joint_re);
       it != std::sregex_iterator(); ++it) {
    auto name = xml_attr((*it)[1].str(), "name");
    if (name) {
      joints.push_back(*name);
    }
  }
  if (!joints.empty()) {
    return joints;
  }

  const std::regex chain_re(R"(<chain\b([^>]*)/?>)");
  std::smatch chain_match;
  if (std::regex_search(body, chain_match, chain_re)) {
    auto base = xml_attr(chain_match[1].str(), "base_link");
    auto tip = xml_attr(chain_match[1].str(), "tip_link");
    if (base && tip) {
      return joints_for_chain_group(urdf_path, *base, *tip);
    }
  }
  return {};
}

bool supported_required_capability(const std::string& capability,
                                   bool snapshot_mode) {
  if (capability == "binary_collision") {
    return true;
  }
  if (!snapshot_mode) {
    return false;
  }
  return capability == "body_pair_only" ||
         capability == "distance" ||
         capability == "penetration_depth";
}

bool timeout_budget_covers_snapshot_sync(core::CollisionSurface surface) {
  switch (surface) {
    case core::CollisionSurface::kObservedRuntimeMonitor:
    case core::CollisionSurface::kObservedRuntimeLookahead:
      return false;
    default:
      return true;
  }
}

void record_timeout_budget(core::CollisionResult& result,
                           const core::CollisionQuery& query) {
  if (query.timeout_budget_ns) {
    result.raw_diagnostics["timeout_budget_ns"] =
        std::to_string(*query.timeout_budget_ns);
  }
}

void record_timeout_exceeded(core::CollisionResult& result,
                             const core::CollisionQuery& query,
                             bool includes_snapshot_sync) {
  record_timeout_budget(result, query);
  result.raw_diagnostics["timeout_includes_snapshot_sync"] =
      includes_snapshot_sync ? "true" : "false";
  if (result.query_duration_ns) {
    result.raw_diagnostics["timeout_observed_duration_ns"] =
        std::to_string(*result.query_duration_ns);
  }
}

Eigen::Matrix4d pose_to_matrix(const core::Pose3DTO& pose) {
  Eigen::Matrix4d tform = Eigen::Matrix4d::Identity();
  tform(0, 3) = pose.position_m.x_m;
  tform(1, 3) = pose.position_m.y_m;
  tform(2, 3) = pose.position_m.z_m;

  const double x = pose.orientation.x;
  const double y = pose.orientation.y;
  const double z = pose.orientation.z;
  const double w = pose.orientation.w;
  tform(0, 0) = 1 - 2 * y * y - 2 * z * z;
  tform(0, 1) = 2 * x * y - 2 * z * w;
  tform(0, 2) = 2 * x * z + 2 * y * w;
  tform(1, 0) = 2 * x * y + 2 * z * w;
  tform(1, 1) = 1 - 2 * x * x - 2 * z * z;
  tform(1, 2) = 2 * y * z - 2 * x * w;
  tform(2, 0) = 2 * x * z - 2 * y * w;
  tform(2, 1) = 2 * y * z + 2 * x * w;
  tform(2, 2) = 1 - 2 * x * x - 2 * y * y;
  return tform;
}

pinocchio::SE3 pose_to_se3(const core::Pose3DTO& pose) {
  const Eigen::Matrix4d tform = pose_to_matrix(pose);
  return pinocchio::SE3(tform.block<3, 3>(0, 0), tform.block<3, 1>(0, 3));
}

pinocchio::SE3 compose_pose_to_se3(const core::Pose3DTO& a,
                                   const core::Pose3DTO& b) {
  const Eigen::Matrix4d tform = pose_to_matrix(a) * pose_to_matrix(b);
  return pinocchio::SE3(tform.block<3, 3>(0, 0), tform.block<3, 1>(0, 3));
}

std::string sanitized_geometry_name(std::string value, std::size_t index) {
  for (char& c : value) {
    const auto uc = static_cast<unsigned char>(c);
    if (!std::isalnum(uc) && c != '_') {
      c = '_';
    }
  }
  if (value.empty() || !std::isalpha(static_cast<unsigned char>(value.front()))) {
    value = "mf_snapshot_" + std::to_string(index) + "_" + value;
  } else {
    value = "mf_snapshot_" + std::to_string(index) + "_" + value;
  }
  return value;
}

std::optional<std::string> link_name_from_parent_body_id(
    const std::string& parent_link_id) {
  constexpr std::string_view kPrefix = "robot/";
  constexpr std::string_view kMiddle = "/link/";
  if (parent_link_id.rfind(std::string(kPrefix), 0) != 0) {
    return std::nullopt;
  }
  const auto middle = parent_link_id.find(std::string(kMiddle), kPrefix.size());
  if (middle == std::string::npos) {
    return std::nullopt;
  }
  const auto begin = middle + kMiddle.size();
  if (begin >= parent_link_id.size()) {
    return std::nullopt;
  }
  const std::string link = parent_link_id.substr(begin);
  if (link.find('/') != std::string::npos) {
    return std::nullopt;
  }
  return link;
}

std::optional<std::string> robot_link_from_collision_body_id(
    const std::string& body_id) {
  constexpr std::string_view kPrefix = "robot/";
  constexpr std::string_view kLink = "/link/";
  constexpr std::string_view kCollision = "/collision/";
  if (body_id.rfind(std::string(kPrefix), 0) != 0) {
    return std::nullopt;
  }
  const auto link_pos = body_id.find(std::string(kLink), kPrefix.size());
  if (link_pos == std::string::npos) {
    return std::nullopt;
  }
  const auto begin = link_pos + kLink.size();
  const auto collision_pos =
      body_id.rfind(std::string(kCollision));
  if (collision_pos == std::string::npos || collision_pos <= begin) {
    return std::nullopt;
  }
  return body_id.substr(begin, collision_pos - begin);
}

bool is_robot_owned_body_id(const std::string& body_id) {
  return body_id.rfind("robot/", 0) == 0;
}

bool is_attached_body_id(const std::string& body_id) {
  return body_id.rfind("robot/", 0) == 0 &&
         body_id.find("/attached/") != std::string::npos;
}

bool is_robot_link_body_id(const std::string& body_id) {
  return robot_link_from_collision_body_id(body_id).has_value();
}

bool link_is_static_wrt_group(const pinocchio::Model& model,
                              const std::string& link_name,
                              const std::vector<std::string>& group_joint_names) {
  if (group_joint_names.empty() || !model.existFrame(link_name)) {
    // Fail operationally conservative for engines constructed without a
    // resolved group: treat robot links as motion-relevant instead of silently
    // pruning them from the collision graph.
    return false;
  }
  auto joint_id = model.frames[model.getFrameId(link_name)].parentJoint;
  while (joint_id > 0 &&
         static_cast<std::size_t>(joint_id) < model.parents.size()) {
    if (static_cast<std::size_t>(joint_id) < model.names.size() &&
        static_cast<std::size_t>(joint_id) < model.nqs.size() &&
        model.nqs[joint_id] > 0 &&
        std::find(group_joint_names.begin(),
                  group_joint_names.end(),
                  model.names[joint_id]) != group_joint_names.end()) {
      return false;
    }
    joint_id = model.parents[joint_id];
  }
  return true;
}

bool body_is_motion_relevant(const std::string& body_id,
                             const pinocchio::Model& model,
                             const std::vector<std::string>& group_joint_names) {
  if (!is_robot_owned_body_id(body_id)) {
    return false;
  }
  if (is_attached_body_id(body_id)) {
    return true;
  }
  auto link = robot_link_from_collision_body_id(body_id);
  if (!link) {
    // Unknown robot-owned body namespace. Keep it in the active graph rather
    // than creating a false negative.
    return true;
  }
  return !link_is_static_wrt_group(model, *link, group_joint_names);
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

std::optional<std::filesystem::path> first_existing_path(
    const std::vector<std::filesystem::path>& candidates) {
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (!candidate.empty() && std::filesystem::exists(candidate, ec)) {
      auto canonical = std::filesystem::canonical(candidate, ec);
      return ec ? candidate : canonical;
    }
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> resolve_mesh_resource_uri(
    const std::string& source_uri,
    const std::vector<std::filesystem::path>& package_paths,
    std::string& error) {
  if (source_uri.empty()) {
    error = "mesh source_uri must not be empty";
    return std::nullopt;
  }

  constexpr const char* kFilePrefix = "file://";
  constexpr const char* kPackagePrefix = "package://";
  std::string path_text = source_uri;
  if (path_text.rfind(kFilePrefix, 0) == 0) {
    path_text = path_text.substr(std::char_traits<char>::length(kFilePrefix));
  } else if (path_text.rfind(kPackagePrefix, 0) == 0) {
    const std::string rest =
        path_text.substr(std::char_traits<char>::length(kPackagePrefix));
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
      error = "invalid package:// mesh URI: " + source_uri;
      return std::nullopt;
    }
    const std::string package_name = rest.substr(0, slash);
    const std::filesystem::path package_rel_path = rest.substr(slash + 1);

    std::vector<std::filesystem::path> candidates;
    for (const auto& prefix : package_paths) {
      candidates.push_back(prefix / package_rel_path);
      candidates.push_back(prefix / package_name / package_rel_path);
      candidates.push_back(prefix / "share" / package_name / package_rel_path);
      if (prefix.filename() == package_name) {
        candidates.push_back(prefix / package_rel_path);
      }
    }
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
    candidates.push_back(std::filesystem::path("/workspace") / package_name /
                         package_rel_path);
    if (auto resolved = first_existing_path(candidates)) {
      return resolved;
    }
    error = "could not resolve package mesh URI: " + source_uri;
    return std::nullopt;
  }

  const std::filesystem::path candidate(path_text);
  std::vector<std::filesystem::path> candidates;
  candidates.push_back(candidate);
  if (candidate.is_relative()) {
    candidates.push_back(std::filesystem::current_path() / candidate);
    candidates.push_back(std::filesystem::path("/workspace") / candidate);
  }
  if (auto resolved = first_existing_path(candidates)) {
    return resolved;
  }
  error = "mesh resource does not exist: " + source_uri;
  return std::nullopt;
}

Eigen::Vector3d effective_mesh_scale(
    const std::optional<core::Vector3DTO>& scale) {
  if (!scale) {
    return Eigen::Vector3d::Ones();
  }
  return Eigen::Vector3d(scale->x_m, scale->y_m, scale->z_m);
}

std::string mesh_cache_key(const std::string& source_uri,
                           const Eigen::Vector3d& scale) {
  return source_uri + "|" + std::to_string(scale.x()) + "|" +
         std::to_string(scale.y()) + "|" + std::to_string(scale.z());
}

std::pair<std::string, std::string> canonical_pair(std::string a,
                                                   std::string b) {
  if (b < a) {
    std::swap(a, b);
  }
  return {std::move(a), std::move(b)};
}

enum class SelectedDistancePairsValidation {
  kValid,
  kMissing,
  kNotCanonical,
  kModeInconsistent,
};

SelectedDistancePairsValidation validate_selected_distance_pairs(
    core::CollisionDistanceRequestMode mode,
    std::span<const core::CollisionPairId> selected_pairs) noexcept {
  if (mode != core::CollisionDistanceRequestMode::kSelectedCollidingPairs) {
    return selected_pairs.empty()
               ? SelectedDistancePairsValidation::kValid
               : SelectedDistancePairsValidation::kModeInconsistent;
  }
  if (selected_pairs.empty()) {
    return SelectedDistancePairsValidation::kMissing;
  }
  for (std::size_t i = 0; i < selected_pairs.size(); ++i) {
    const auto& pair = selected_pairs[i];
    if (pair.body_a.empty() || pair.body_b.empty() ||
        !(pair.body_a < pair.body_b)) {
      return SelectedDistancePairsValidation::kNotCanonical;
    }
    if (i > 0 && !(selected_pairs[i - 1] < pair)) {
      return SelectedDistancePairsValidation::kNotCanonical;
    }
  }
  return SelectedDistancePairsValidation::kValid;
}

const char* selected_distance_pairs_validation_reason(
    SelectedDistancePairsValidation validation) noexcept {
  switch (validation) {
    case SelectedDistancePairsValidation::kValid:
      return "none";
    case SelectedDistancePairsValidation::kMissing:
      return "selected_distance_pairs_missing";
    case SelectedDistancePairsValidation::kNotCanonical:
      return "selected_distance_pairs_not_canonical";
    case SelectedDistancePairsValidation::kModeInconsistent:
      return "distance_request_mode_inconsistent";
  }
  return "distance_request_mode_inconsistent";
}

bool pair_is_selected(
    const core::CollisionPairId& pair,
    std::span<const core::CollisionPairId> selected_pairs) noexcept {
  return std::binary_search(selected_pairs.begin(), selected_pairs.end(), pair);
}

core::CollisionDistanceRequestMode effective_distance_request_mode(
    const core::CollisionQuery& query) noexcept {
  return query.distance_request_mode;
}

void reset_distance_warm_start(coal::DistanceRequest& request) {
  // Pinocchio enables Coal's mutable per-pair GJK warm start in GeometryData.
  // Reusing that history makes an identical signed penetration depend on how
  // many earlier samples one worker processed. Reset both hints before every
  // policy-visible distance evaluation so independent workers start from the
  // same deterministic support state.
  request.cached_gjk_guess = coal::Vec3s(1.0, 0.0, 0.0);
  request.cached_support_func_guess = coal::support_func_guess_t::Zero();
}

bool requests_distance_capability(const core::CollisionQuery& query) noexcept {
  return std::find(query.required_capabilities.begin(),
                   query.required_capabilities.end(),
                   "distance") != query.required_capabilities.end() ||
         std::find(query.required_capabilities.begin(),
                   query.required_capabilities.end(),
                   "penetration_depth") != query.required_capabilities.end();
}

std::map<std::string, core::CollisionBodyId> build_raw_to_body_map(
    const pinocchio::Model& model,
    const pinocchio::GeometryModel& collision_model,
    const core::CollisionWorldSnapshot& snapshot,
    const std::vector<std::string>& synced_scene_geometry_names,
    const std::map<std::string, core::CollisionBodyId>& synced_raw_name_to_body_id) {
  std::map<std::string, core::CollisionBodyId> raw_to_body =
      synced_raw_name_to_body_id;

  std::set<std::string> scene_raw_names;
  for (const auto& name : synced_scene_geometry_names) {
    scene_raw_names.insert(name);
  }
  std::map<std::string, std::map<int, core::CollisionBodyId>>
      robot_ids_by_link_index;
  for (const auto& body : snapshot.bodies) {
    if (body.owner_kind != core::CollisionOwnerKind::kRobotLink) {
      continue;
    }
    auto link = robot_link_from_collision_body_id(body.body_id);
    if (!link) {
      continue;
    }
    const auto suffix = body.body_id.rfind("/collision/");
    int index = 0;
    if (suffix != std::string::npos) {
      index = std::stoi(body.body_id.substr(suffix + 11));
    }
    robot_ids_by_link_index[*link][index] = body.body_id;
  }

  std::map<std::string, int> next_collision_index_by_link;
  for (const auto& geometry_object : collision_model.geometryObjects) {
    if (scene_raw_names.find(geometry_object.name) != scene_raw_names.end()) {
      continue;
    }
    if (geometry_object.parentFrame >= model.frames.size()) {
      continue;
    }
    const auto link_name = model.frames[geometry_object.parentFrame].name;
    const int index = next_collision_index_by_link[link_name]++;
    auto link_it = robot_ids_by_link_index.find(link_name);
    if (link_it == robot_ids_by_link_index.end()) {
      continue;
    }
    auto body_it = link_it->second.find(index);
    if (body_it == link_it->second.end()) {
      continue;
    }
    raw_to_body[geometry_object.name] = body_it->second;
    if (index == 0) {
      raw_to_body[link_name] = body_it->second;
    }
  }
  return raw_to_body;
}

}  // namespace

struct ManyForgeCollisionEngine::SharedModel {
  std::string model_name;
  pinocchio::Model model;
  pinocchio::GeometryModel base_collision_model;
  std::vector<std::filesystem::path> package_paths;
  std::vector<std::string> group_joint_names;
  std::string group_name;
};

struct ManyForgeCollisionEngine::PreparedScene {
  struct RuntimePairBinding {
    bool mapped = false;
    core::CollisionPairId pair;
  };

  core::CollisionWorldRevision revision;
  pinocchio::GeometryModel collision_model;
  std::vector<std::string> scene_geometry_names;
  std::map<std::string, core::CollisionBodyId> raw_name_to_body_id;
  std::map<std::string, core::CollisionBodyId> raw_to_body_map;
  std::vector<RuntimePairBinding> runtime_pair_bindings;
};

struct ManyForgeCollisionEngine::SharedSceneCache {
  explicit SharedSceneCache(SharedModelPtr model)
      : shared_model(std::move(model)) {}

  SharedModelPtr shared_model;
  std::atomic<std::shared_ptr<const PreparedScene>> latest;
  std::atomic<std::uint64_t> build_count{0};
  std::mutex mesh_cache_mutex;
  std::map<std::string, std::shared_ptr<coal::CollisionGeometry>>
      mesh_geometry_cache;
};

ManyForgeCollisionEngine::SharedModelPtr
ManyForgeCollisionEngine::load_shared_model(
    const std::string& model_name,
    const std::filesystem::path& urdf_path,
    const std::filesystem::path& srdf_path,
    const std::vector<std::filesystem::path>& package_paths,
    std::string group_name) {
  auto shared = std::make_shared<SharedModel>();
  shared->model_name = model_name;
  shared->package_paths = package_paths;
  shared->group_name = std::move(group_name);
  pinocchio::urdf::buildModel(urdf_path.string(), shared->model,
                              /*verbose=*/false,
                              /*mimic=*/true);
  std::vector<std::string> package_path_strings;
  package_path_strings.reserve(package_paths.size());
  for (const auto& path : package_paths) {
    package_path_strings.push_back(path.string());
  }
  pinocchio::urdf::buildGeom(shared->model, urdf_path.string(),
                             pinocchio::COLLISION,
                             shared->base_collision_model,
                             package_path_strings);
  if (!srdf_path.empty() && std::filesystem::exists(srdf_path)) {
    pinocchio::srdf::removeCollisionPairs(shared->model,
                                          shared->base_collision_model,
                                          srdf_path.string(), false);
  }
  shared->group_joint_names =
      group_joint_names_from_srdf(srdf_path, urdf_path, shared->group_name);
  return shared;
}

ManyForgeCollisionEngine::SharedSceneCachePtr
ManyForgeCollisionEngine::make_shared_scene_cache(SharedModelPtr shared_model) {
  if (!shared_model) {
    throw std::invalid_argument("collision scene cache requires a shared model");
  }
  return std::make_shared<SharedSceneCache>(std::move(shared_model));
}

ManyForgeCollisionEngine::ManyForgeCollisionEngine(
    const std::string& model_name,
    const std::filesystem::path& urdf_path,
    const std::filesystem::path& srdf_path,
    const std::vector<std::filesystem::path>& package_paths,
    std::string group_name)
    : ManyForgeCollisionEngine(
          load_shared_model(model_name, urdf_path, srdf_path, package_paths,
                            std::move(group_name)),
          nullptr) {}

ManyForgeCollisionEngine::ManyForgeCollisionEngine(
    SharedModelPtr shared_model, SharedSceneCachePtr scene_cache)
    : shared_model_(std::move(shared_model)),
      scene_cache_(std::move(scene_cache)),
      engine_id_(shared_model_ ? "manyforge_collision/" + shared_model_->model_name
                               : "manyforge_collision/invalid"),
      engine_version_(MANYFORGE_COLLISION_ENGINE_VERSION) {
  if (!shared_model_) {
    throw std::invalid_argument("collision engine requires a shared model");
  }
  if (!scene_cache_) {
    scene_cache_ = make_shared_scene_cache(shared_model_);
  }
  if (scene_cache_->shared_model.get() != shared_model_.get()) {
    throw std::invalid_argument(
        "collision engine scene cache belongs to a different shared model");
  }
  reusable_model_data_.emplace(shared_model_->model);

  capability_.engine_id = engine_id_;
  capability_.engine_version = engine_version_;
  capability_.library_lineage = "manyforge_collision_pinocchio_coal";
  capability_.mutable_context_id =
      engine_id_ + "/context/" +
      std::to_string(reinterpret_cast<std::uintptr_t>(this));
  capability_.supported_geometry_kinds = {
      "box", "sphere", "cylinder", "triangle_mesh"};
  capability_.supports_distance_query = true;
  capability_.supported_distance_request_modes = {
      core::CollisionDistanceRequestMode::kNone,
      core::CollisionDistanceRequestMode::kSelectedCollidingPairs,
      core::CollisionDistanceRequestMode::kAllCollidingPairs,
      core::CollisionDistanceRequestMode::kAllActivePairs};
  capability_.supports_contact_points = false;
  capability_.supports_penetration_depth = true;
  capability_.supports_continuous_collision = false;
  capability_.thread_safety =
      core::CollisionEngineThreadSafetyClass::kSingleContextExclusive;
  capability_.deterministic_ordering = true;
  capability_.runtime_query_memory_model =
      core::RuntimeCollisionQueryMemoryModel::kMeasuredBounded;
  capability_.runtime_query_max_joints = 256;
  capability_.runtime_query_max_world_bodies = 160;
  capability_.runtime_query_max_collision_pairs = 2048;
  capability_.runtime_query_clean_allocation_upper_bound = 4096;
  capability_.runtime_query_snapshot_change_may_allocate = true;
  capability_.runtime_query_snapshot_change_allocation_upper_bound = 65'536;
  capability_.runtime_query_qualified_wcet_ns = 10'000'000;
  capability_.runtime_query_snapshot_change_qualified_wcet_ns = 50'000'000;
  capability_.known_unsupported_cases = {
      "convex_mesh_without_qualified_decomposition",
      "scene_object_compound_geometry",
      "contact_points",
      "continuous_collision"};
}

const void* ManyForgeCollisionEngine::internal_model_identity() const {
  return static_cast<const void*>(shared_model_.get());
}

const void* ManyForgeCollisionEngine::internal_scene_cache_identity() const {
  return static_cast<const void*>(scene_cache_.get());
}

const void* ManyForgeCollisionEngine::internal_prepared_scene_identity() const {
  std::lock_guard<std::mutex> lock(scene_mutex_);
  return static_cast<const void*>(active_prepared_scene_.get());
}

std::uint64_t ManyForgeCollisionEngine::internal_prepared_scene_build_count() const {
  return scene_cache_->build_count.load(std::memory_order_acquire);
}

ManyForgeCollisionEngine::QueryMechanismCounters
ManyForgeCollisionEngine::internal_last_query_mechanism_counters() const {
  std::lock_guard<std::mutex> lock(scene_mutex_);
  return last_query_mechanism_counters_;
}

core::RuntimeCollisionQueryPrepareStatus
ManyForgeCollisionEngine::prepare_runtime_query(
    std::span<const std::string> canonical_joint_names) noexcept {
  try {
    if (canonical_joint_names.empty() ||
        canonical_joint_names.size() > capability_.runtime_query_max_joints) {
      return canonical_joint_names.empty()
                 ? core::RuntimeCollisionQueryPrepareStatus::kInvalidJointOrder
                 : core::RuntimeCollisionQueryPrepareStatus::kJointCountExceeded;
    }
    std::lock_guard<std::mutex> scene_lock(scene_mutex_);
    runtime_joint_q_indices_.clear();
    runtime_joint_q_indices_.reserve(canonical_joint_names.size());
    for (const auto& joint_name : canonical_joint_names) {
      if (!shared_model_->model.existJointName(joint_name)) {
        runtime_query_prepared_ = false;
        return core::RuntimeCollisionQueryPrepareStatus::kInvalidJointOrder;
      }
      const auto joint_id = shared_model_->model.getJointId(joint_name);
      if (joint_id >= shared_model_->model.idx_qs.size() ||
          joint_id >= shared_model_->model.nqs.size() ||
          shared_model_->model.nqs[joint_id] != 1) {
        runtime_query_prepared_ = false;
        return core::RuntimeCollisionQueryPrepareStatus::kInvalidJointOrder;
      }
      const auto q_index = shared_model_->model.idx_qs[joint_id];
      if (std::find(runtime_joint_q_indices_.begin(),
                    runtime_joint_q_indices_.end(), q_index) !=
          runtime_joint_q_indices_.end()) {
        runtime_query_prepared_ = false;
        return core::RuntimeCollisionQueryPrepareStatus::kInvalidJointOrder;
      }
      runtime_joint_q_indices_.push_back(q_index);
    }
    runtime_query_is_full_configuration_ =
        static_cast<Eigen::Index>(canonical_joint_names.size()) ==
        shared_model_->model.nq;
    if (runtime_query_is_full_configuration_) {
      for (std::size_t i = 0; i < runtime_joint_q_indices_.size(); ++i) {
        if (runtime_joint_q_indices_[i] != i) {
          runtime_query_is_full_configuration_ = false;
          break;
        }
      }
    }
    runtime_q_neutral_ = pinocchio::neutral(shared_model_->model);
    runtime_q_scratch_ = runtime_q_neutral_;
    runtime_query_prepared_ = true;
    return core::RuntimeCollisionQueryPrepareStatus::kReady;
  } catch (...) {
    runtime_query_prepared_ = false;
    return core::RuntimeCollisionQueryPrepareStatus::kEngineError;
  }
}

core::RuntimeCollisionQueryPrepareStatus
ManyForgeCollisionEngine::prepare_runtime_snapshot(
    const core::CollisionWorldSnapshot& snapshot) noexcept {
  try {
    if (snapshot.bodies.size() > capability_.runtime_query_max_world_bodies) {
      return core::RuntimeCollisionQueryPrepareStatus::kEngineError;
    }
    std::lock_guard<std::mutex> scene_lock(scene_mutex_);
    if (auto error = sync_snapshot_locked(snapshot, /*allow_resource_io=*/true)) {
      return core::RuntimeCollisionQueryPrepareStatus::kEngineError;
    }
    return core::RuntimeCollisionQueryPrepareStatus::kReady;
  } catch (...) {
    return core::RuntimeCollisionQueryPrepareStatus::kEngineError;
  }
}

core::RuntimeCollisionQueryResultView ManyForgeCollisionEngine::check_runtime(
    const core::CollisionWorldSnapshot& snapshot,
    const core::RuntimeCollisionQueryRequest& request) noexcept {
  const auto started = std::chrono::steady_clock::now();
  core::RuntimeCollisionQueryResultView out;
  const auto finish_without_evidence =
      [&](core::RuntimeCollisionQueryStatus status,
          core::RuntimeCollisionQueryFailureReason reason,
          core::CollisionEngineStatus engine_status) {
    out.status = status;
    out.failure_reason = reason;
    out.engine_status = engine_status;
    out.query_duration_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started)
            .count();
    return out;
  };
  if (snapshot.bodies.size() > capability_.runtime_query_max_world_bodies) {
    return finish_without_evidence(
        core::RuntimeCollisionQueryStatus::kInconclusive,
        core::RuntimeCollisionQueryFailureReason::kSnapshotBodyLimitExceeded,
        core::CollisionEngineStatus::kDiagnosticIncomplete);
  }
  if (request.timeout_budget_ns <= 0) {
    return finish_without_evidence(
        core::RuntimeCollisionQueryStatus::kInconclusive,
        core::RuntimeCollisionQueryFailureReason::kTimeout,
        core::CollisionEngineStatus::kTimeout);
  }

  try {
    std::unique_lock<std::mutex> scene_lock(scene_mutex_, std::try_to_lock);
    if (!scene_lock.owns_lock()) {
      // Another owner may be changing the scratch vectors while holding the
      // mutex. A busy result therefore exposes no view into those vectors.
      return finish_without_evidence(
          core::RuntimeCollisionQueryStatus::kInconclusive,
          core::RuntimeCollisionQueryFailureReason::kEngineBusy,
          core::CollisionEngineStatus::kTimeout);
    }
    runtime_collision_observations_.clear();
    runtime_clearance_observations_.clear();
    last_query_mechanism_counters_ = {};
    last_query_mechanism_counters_.distance_request_mode =
        request.distance_request_mode;
    auto finish = [&](core::RuntimeCollisionQueryStatus status,
                      core::RuntimeCollisionQueryFailureReason reason,
                      core::CollisionEngineStatus engine_status) {
      auto result = finish_without_evidence(status, reason, engine_status);
      result.collision_pairs = runtime_collision_observations_;
      result.clearance_pairs = runtime_clearance_observations_;
      return result;
    };
    if (validate_selected_distance_pairs(request.distance_request_mode,
                                         request.selected_distance_pairs) !=
        SelectedDistancePairsValidation::kValid) {
      return finish(
          core::RuntimeCollisionQueryStatus::kInconclusive,
          core::RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete,
          core::CollisionEngineStatus::kDiagnosticIncomplete);
    }
    if (!runtime_query_prepared_) {
      return finish(core::RuntimeCollisionQueryStatus::kInconclusive,
                    core::RuntimeCollisionQueryFailureReason::kUnsupported,
                    core::CollisionEngineStatus::kDiagnosticIncomplete);
    }
    const std::size_t expected_positions =
        runtime_query_is_full_configuration_
            ? static_cast<std::size_t>(shared_model_->model.nq)
            : runtime_joint_q_indices_.size();
    if (request.positions_rad.size() != expected_positions) {
      return finish(
          core::RuntimeCollisionQueryStatus::kInconclusive,
          core::RuntimeCollisionQueryFailureReason::kObservedStateShapeMismatch,
          core::CollisionEngineStatus::kDiagnosticIncomplete);
    }
    if (!std::all_of(request.positions_rad.begin(), request.positions_rad.end(),
                     [](double value) { return std::isfinite(value); })) {
      return finish(
          core::RuntimeCollisionQueryStatus::kInconclusive,
          core::RuntimeCollisionQueryFailureReason::kObservedStateNotFinite,
          core::CollisionEngineStatus::kDiagnosticIncomplete);
    }
    if (auto error =
            sync_snapshot_locked(snapshot, /*allow_resource_io=*/false)) {
      (void)error;
      return finish(
          core::RuntimeCollisionQueryStatus::kInconclusive,
          core::RuntimeCollisionQueryFailureReason::kSnapshotSyncFailed,
          core::CollisionEngineStatus::kDiagnosticIncomplete);
    }
    const auto prepared_scene = active_prepared_scene_;
    const bool distance_requested =
        request.distance_request_mode !=
        core::CollisionDistanceRequestMode::kNone;
    if (!prepared_scene || !active_collision_data_ ||
        prepared_scene->runtime_pair_bindings.size() !=
            prepared_scene->collision_model.collisionPairs.size() ||
        active_collision_data_->collisionResults.size() !=
            prepared_scene->collision_model.collisionPairs.size() ||
        (distance_requested &&
         (active_collision_data_->distanceResults.size() !=
              prepared_scene->collision_model.collisionPairs.size() ||
          active_collision_data_->distanceRequests.size() !=
              prepared_scene->collision_model.collisionPairs.size()))) {
      return finish(
          core::RuntimeCollisionQueryStatus::kInconclusive,
          core::RuntimeCollisionQueryFailureReason::kSnapshotInvalid,
          core::CollisionEngineStatus::kDiagnosticIncomplete);
    }

    const auto pair_count =
        prepared_scene->collision_model.collisionPairs.size();
    if (runtime_collision_observations_.capacity() < pair_count ||
        (distance_requested &&
         runtime_clearance_observations_.capacity() < pair_count)) {
      return finish(
          core::RuntimeCollisionQueryStatus::kInconclusive,
          core::RuntimeCollisionQueryFailureReason::kObservationCapacityExceeded,
          core::CollisionEngineStatus::kDiagnosticIncomplete);
    }

    if (runtime_query_is_full_configuration_) {
      for (std::size_t i = 0; i < request.positions_rad.size(); ++i) {
        runtime_q_scratch_(static_cast<Eigen::Index>(i)) =
            request.positions_rad[i];
      }
    } else {
      runtime_q_scratch_ = runtime_q_neutral_;
      for (std::size_t i = 0; i < request.positions_rad.size(); ++i) {
        runtime_q_scratch_(runtime_joint_q_indices_[i]) =
            request.positions_rad[i];
      }
    }

    const bool in_collision = pinocchio::computeCollisions(
        shared_model_->model, *reusable_model_data_,
        prepared_scene->collision_model,
        *active_collision_data_, runtime_q_scratch_,
        /*stop_at_first_collision=*/false);
    last_query_mechanism_counters_.collision_api_call_count = 1;
    last_query_mechanism_counters_
        .collision_enumeration_completion_ordinal = 1;
    last_query_mechanism_counters_.fk_placement_invocation_count = 1;

    const auto pair_equal = [](const core::CollisionPairId& lhs,
                               const core::CollisionPairId& rhs) {
      return lhs == rhs;
    };
    const auto observation_less = [](const auto& lhs, const auto& rhs) {
      return *lhs.pair < *rhs.pair;
    };
    auto find_collision_observation = [&](const core::CollisionPairId& pair) {
      return std::find_if(
          runtime_collision_observations_.begin(),
          runtime_collision_observations_.end(), [&](const auto& observation) {
            return observation.pair != nullptr &&
                   pair_equal(*observation.pair, pair);
          });
    };
    auto merge_finite_distance = [&](const core::CollisionPairId* pair,
                                     double distance) {
      if (pair == nullptr || !std::isfinite(distance)) {
        return;
      }
      auto clearance = std::find_if(
          runtime_clearance_observations_.begin(),
          runtime_clearance_observations_.end(), [&](const auto& observation) {
            return observation.pair != nullptr &&
                   pair_equal(*observation.pair, *pair);
          });
      if (clearance == runtime_clearance_observations_.end()) {
        runtime_clearance_observations_.push_back({pair, distance});
      } else if (!clearance->signed_clearance_m ||
                 distance < *clearance->signed_clearance_m) {
        clearance->signed_clearance_m = distance;
      }
      auto collision = find_collision_observation(*pair);
      if (collision != runtime_collision_observations_.end() &&
          (!collision->signed_clearance_m ||
           distance < *collision->signed_clearance_m)) {
        collision->signed_clearance_m = distance;
      }
    };

    // First materialize the complete colliding logical-pair set. Distance scope
    // must never alter collision coverage.
    for (std::size_t i = 0; i < pair_count; ++i) {
      if (i >= active_collision_data_->activeCollisionPairs.size() ||
          !active_collision_data_->activeCollisionPairs[i]) {
        continue;
      }
      ++last_query_mechanism_counters_.active_collision_pair_count;
      if (!active_collision_data_->collisionResults[i].isCollision()) {
        continue;
      }
      ++last_query_mechanism_counters_.colliding_geometry_pair_count;
      if (i >= prepared_scene->runtime_pair_bindings.size() ||
          !prepared_scene->runtime_pair_bindings[i].mapped) {
        return finish(
            core::RuntimeCollisionQueryStatus::kInconclusive,
            core::RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete,
            core::CollisionEngineStatus::kDiagnosticIncomplete);
      }
      const auto* pair = &prepared_scene->runtime_pair_bindings[i].pair;
      if (find_collision_observation(*pair) ==
          runtime_collision_observations_.end()) {
        runtime_collision_observations_.push_back({pair, std::nullopt});
      }
    }
    std::sort(runtime_collision_observations_.begin(),
              runtime_collision_observations_.end(), observation_less);
    last_query_mechanism_counters_.colliding_logical_pair_count =
        runtime_collision_observations_.size();
    if (in_collision && runtime_collision_observations_.empty()) {
      return finish(
          core::RuntimeCollisionQueryStatus::kInconclusive,
          core::RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete,
          core::CollisionEngineStatus::kDiagnosticIncomplete);
    }

    if (request.distance_request_mode ==
        core::CollisionDistanceRequestMode::kAllActivePairs) {
      last_query_mechanism_counters_.first_distance_evaluation_ordinal = 2;
      for (std::size_t i = 0; i < pair_count; ++i) {
        if (i < active_collision_data_->activeCollisionPairs.size() &&
            active_collision_data_->activeCollisionPairs[i]) {
          reset_distance_warm_start(
              active_collision_data_->distanceRequests[i]);
          active_collision_data_->distanceRequests[i].enable_signed_distance =
              true;
        }
      }
      pinocchio::computeDistances(shared_model_->model, *reusable_model_data_,
                                  prepared_scene->collision_model,
                                  *active_collision_data_, runtime_q_scratch_);
      last_query_mechanism_counters_.distance_api_call_count = 1;
      ++last_query_mechanism_counters_.fk_placement_invocation_count;

      for (std::size_t i = 0; i < pair_count; ++i) {
        if (i >= active_collision_data_->activeCollisionPairs.size() ||
            !active_collision_data_->activeCollisionPairs[i]) {
          continue;
        }
        ++last_query_mechanism_counters_
              .distance_evaluated_geometry_pair_count;
        if (i >= prepared_scene->runtime_pair_bindings.size() ||
            !prepared_scene->runtime_pair_bindings[i].mapped) {
          // Preserve the legacy runtime behavior: an unmapped non-colliding
          // active distance pair is omitted, while any colliding pair already
          // failed closed during complete collision materialization above.
          continue;
        }
        const auto* pair = &prepared_scene->runtime_pair_bindings[i].pair;
        bool first_logical_pair = true;
        for (std::size_t prior = 0; prior < i; ++prior) {
          if (prior < active_collision_data_->activeCollisionPairs.size() &&
              active_collision_data_->activeCollisionPairs[prior] &&
              prepared_scene->runtime_pair_bindings[prior].mapped &&
              prepared_scene->runtime_pair_bindings[prior].pair == *pair) {
            first_logical_pair = false;
            break;
          }
        }
        if (first_logical_pair) {
          ++last_query_mechanism_counters_
                .distance_evaluated_logical_pair_count;
        }
        merge_finite_distance(
            pair, active_collision_data_->distanceResults[i].min_distance);
      }
    } else if (request.distance_request_mode ==
                   core::CollisionDistanceRequestMode::kAllCollidingPairs ||
               request.distance_request_mode ==
                   core::CollisionDistanceRequestMode::kSelectedCollidingPairs) {
      bool selected_distance_evidence_complete = true;
      for (const auto& collision : runtime_collision_observations_) {
        if (request.distance_request_mode ==
                core::CollisionDistanceRequestMode::kAllCollidingPairs ||
            pair_is_selected(*collision.pair,
                             request.selected_distance_pairs)) {
          ++last_query_mechanism_counters_
                .distance_evaluated_logical_pair_count;
        }
      }
      for (std::size_t i = 0; i < pair_count; ++i) {
        if (i >= active_collision_data_->activeCollisionPairs.size() ||
            !active_collision_data_->activeCollisionPairs[i] ||
            !active_collision_data_->collisionResults[i].isCollision()) {
          continue;
        }
        const auto& binding = prepared_scene->runtime_pair_bindings[i];
        const bool selected =
            request.distance_request_mode ==
                core::CollisionDistanceRequestMode::kAllCollidingPairs ||
            pair_is_selected(binding.pair, request.selected_distance_pairs);
        if (!selected) {
          continue;
        }
        if (last_query_mechanism_counters_.distance_api_call_count == 0) {
          last_query_mechanism_counters_.first_distance_evaluation_ordinal = 2;
        }
        reset_distance_warm_start(
            active_collision_data_->distanceRequests[i]);
        active_collision_data_->distanceRequests[i].enable_signed_distance =
            true;
        auto& distance_result = pinocchio::computeDistance(
            prepared_scene->collision_model, *active_collision_data_, i);
        ++last_query_mechanism_counters_.distance_api_call_count;
        ++last_query_mechanism_counters_
              .distance_evaluated_geometry_pair_count;
        detail::SelectedCollidingDistanceEvidence selected_evidence;
        selected_evidence.observe(distance_result.min_distance);
        if (!selected_evidence.complete ||
            !selected_evidence.deepest_finite_m) {
          selected_distance_evidence_complete = false;
          continue;
        }
        merge_finite_distance(&binding.pair,
                              *selected_evidence.deepest_finite_m);
      }

      if (!selected_distance_evidence_complete) {
        for (auto& observation : runtime_collision_observations_) {
          observation.signed_clearance_m.reset();
        }
        runtime_clearance_observations_.clear();
        return finish(
            core::RuntimeCollisionQueryStatus::kInconclusive,
            core::RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete,
            core::CollisionEngineStatus::kDiagnosticIncomplete);
      }

      // Every selected colliding geometry component must have contributed
      // finite evidence; only then may the deepest logical metric be used.
      for (const auto& collision : runtime_collision_observations_) {
        const bool selected =
            request.distance_request_mode ==
                core::CollisionDistanceRequestMode::kAllCollidingPairs ||
            pair_is_selected(*collision.pair,
                             request.selected_distance_pairs);
        if (selected &&
            (!collision.signed_clearance_m ||
             *collision.signed_clearance_m > 0.0)) {
          return finish(
              core::RuntimeCollisionQueryStatus::kInconclusive,
              core::RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete,
              core::CollisionEngineStatus::kDiagnosticIncomplete);
        }
      }
    }
    std::sort(runtime_clearance_observations_.begin(),
              runtime_clearance_observations_.end(), observation_less);

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started)
            .count();
    if (elapsed > request.timeout_budget_ns) {
      return finish(core::RuntimeCollisionQueryStatus::kInconclusive,
                    core::RuntimeCollisionQueryFailureReason::kTimeout,
                    core::CollisionEngineStatus::kTimeout);
    }
    return finish(in_collision ? core::RuntimeCollisionQueryStatus::kCollision
                               : core::RuntimeCollisionQueryStatus::kClear,
                  core::RuntimeCollisionQueryFailureReason::kNone,
                  core::CollisionEngineStatus::kOk);
  } catch (...) {
    return finish_without_evidence(
        core::RuntimeCollisionQueryStatus::kInconclusive,
        core::RuntimeCollisionQueryFailureReason::kEngineException,
        core::CollisionEngineStatus::kEngineError);
  }
}

core::CollisionResult ManyForgeCollisionEngine::check(
    const core::CollisionWorldSnapshot& snapshot,
    const core::CollisionQuery& query,
    const core::CollisionPolicySnapshot& policy,
    const core::CollisionCancellationToken& cancel) {
  static_cast<void>(policy);
  core::CollisionResult result;
  result.query_id = query.query_id;
  result.surface = query.surface;
  result.policy_revision = query.policy_revision;
  result.state_time_ns = query.state_time_ns;
  result.sampling_mode = query.sampling_mode;
  result.engine_id = engine_id_;
  result.engine_version = engine_version_;
  result.collision_world_revision = snapshot.revision;

  const auto t_start = std::chrono::steady_clock::now();
  auto elapsed_ns_since =
      [](const std::chrono::steady_clock::time_point& start) {
        const auto t_end = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t_end -
                                                                    start)
            .count();
      };
  auto finish_with_duration = [&]() {
    result.query_duration_ns = elapsed_ns_since(t_start);
  };

  if (cancel.is_cancel_requested()) {
    finish_with_duration();
    result.engine_status = core::CollisionEngineStatus::kCancelled;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    result.raw_diagnostics["cancel_reason"] = cancel.reason();
    return result;
  }
  if (snapshot.snapshot_format_version != 1 || snapshot.revision.revision_id == 0) {
    finish_with_duration();
    result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    result.raw_diagnostics["snapshot_format_version"] =
        std::to_string(snapshot.snapshot_format_version);
    return result;
  }
  if (query.collision_world_revision.revision_id != 0 &&
      query.collision_world_revision.revision_id != snapshot.revision.revision_id) {
    finish_with_duration();
    result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    result.raw_diagnostics["query_revision_id"] =
        std::to_string(query.collision_world_revision.revision_id);
    result.raw_diagnostics["snapshot_revision_id"] =
        std::to_string(snapshot.revision.revision_id);
    return result;
  }
  for (const auto& capability : query.required_capabilities) {
    if (!supported_required_capability(capability, true)) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["required_capability"] = capability;
      return result;
    }
  }
  if (query.timeout_budget_ns && *query.timeout_budget_ns <= 0) {
    finish_with_duration();
    record_timeout_budget(result, query);
    result.engine_status = core::CollisionEngineStatus::kTimeout;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    return result;
  }

  try {
    std::lock_guard<std::mutex> scene_lock(scene_mutex_);
    const auto sync_start = std::chrono::steady_clock::now();
    if (auto sync_error = sync_snapshot_locked(snapshot, /*allow_resource_io=*/true);
        sync_error.has_value()) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kUnsupportedGeometry;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["snapshot_sync_error"] = *sync_error;
      result.raw_diagnostics["snapshot_sync_duration_ns"] =
          std::to_string(elapsed_ns_since(sync_start));
      return result;
    }
    const auto snapshot_sync_duration_ns = elapsed_ns_since(sync_start);
    if (cancel.is_cancel_requested()) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kCancelled;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["cancel_reason"] = cancel.reason();
      result.raw_diagnostics["snapshot_sync_duration_ns"] =
          std::to_string(snapshot_sync_duration_ns);
      return result;
    }
    if (timeout_budget_covers_snapshot_sync(query.surface) &&
        query.timeout_budget_ns &&
        snapshot_sync_duration_ns > *query.timeout_budget_ns) {
      result.raw_diagnostics["snapshot_sync_duration_ns"] =
          std::to_string(snapshot_sync_duration_ns);
      result.raw_diagnostics["total_call_duration_ns"] =
          std::to_string(elapsed_ns_since(t_start));
      result.query_duration_ns = snapshot_sync_duration_ns;
      record_timeout_exceeded(result, query,
                              /*includes_snapshot_sync=*/true);
      result.engine_status = core::CollisionEngineStatus::kTimeout;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      return result;
    }
    result = check_locked(query, &snapshot, &cancel);
    const auto total_call_duration_ns = elapsed_ns_since(t_start);
    result.raw_diagnostics["snapshot_sync_duration_ns"] =
        std::to_string(snapshot_sync_duration_ns);
    result.raw_diagnostics["total_call_duration_ns"] =
        std::to_string(total_call_duration_ns);
    // Offline validation treats snapshot ingestion as part of the blocking
    // validation call. Observed runtime surfaces have a different contract:
    // the per-state budget measures the already-synchronized collision query,
    // while each owning session independently bounds the complete tick/job and
    // verifies scene freshness. Counting cold snapshot ingestion as a query
    // timeout causes false runtime interrupts when a live object publishes a
    // new revision with no robot collision.
    if (timeout_budget_covers_snapshot_sync(query.surface)) {
      result.query_duration_ns = total_call_duration_ns;
    }
    if ((result.engine_status == core::CollisionEngineStatus::kOk ||
         result.engine_status == core::CollisionEngineStatus::kTimeout) &&
        query.timeout_budget_ns &&
        result.query_duration_ns &&
        *result.query_duration_ns > *query.timeout_budget_ns) {
      record_timeout_exceeded(
          result, query, timeout_budget_covers_snapshot_sync(query.surface));
      result.engine_status = core::CollisionEngineStatus::kTimeout;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      return result;
    }
  } catch (const std::exception& exc) {
    finish_with_duration();
    result.engine_status = core::CollisionEngineStatus::kEngineError;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    result.raw_diagnostics["engine_exception"] = exc.what();
    return result;
  } catch (...) {
    finish_with_duration();
    result.engine_status = core::CollisionEngineStatus::kEngineError;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    return result;
  }
  return result;
}

core::CollisionResult ManyForgeCollisionEngine::check_locked(
    const core::CollisionQuery& query,
    const core::CollisionWorldSnapshot* snapshot,
    const core::CollisionCancellationToken* cancel) {
  core::CollisionResult result;
  result.query_id = query.query_id;
  result.surface = query.surface;
  result.policy_revision = query.policy_revision;
  result.state_time_ns = query.state_time_ns;
  result.sampling_mode = query.sampling_mode;
  result.engine_id = engine_id_;
  result.engine_version = engine_version_;
  if (snapshot != nullptr) {
    result.collision_world_revision = snapshot->revision;
  } else if (query.collision_world_revision.revision_id != 0 ||
             !query.collision_world_revision.content_hash.empty()) {
    result.collision_world_revision = query.collision_world_revision;
  }

  const auto distance_request_mode = effective_distance_request_mode(query);
  last_query_mechanism_counters_ = {};
  last_query_mechanism_counters_.distance_request_mode =
      distance_request_mode;

  const auto t_start = std::chrono::steady_clock::now();
  auto finish_with_duration = [&]() {
    const auto t_end = std::chrono::steady_clock::now();
    result.query_duration_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start)
            .count();
  };

  const auto selected_pair_validation = validate_selected_distance_pairs(
      distance_request_mode,
      std::span<const core::CollisionPairId>(query.selected_distance_pairs));
  if (selected_pair_validation != SelectedDistancePairsValidation::kValid) {
    finish_with_duration();
    result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    result.raw_diagnostics["distance_scope_error"] =
        selected_distance_pairs_validation_reason(selected_pair_validation);
    return result;
  }
  if (distance_request_mode ==
          core::CollisionDistanceRequestMode::kNone &&
      requests_distance_capability(query)) {
    finish_with_duration();
    result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    result.raw_diagnostics["distance_scope_error"] =
        "distance or penetration_depth capability requires an explicit typed "
        "distance request scope";
    return result;
  }

  for (const auto& capability : query.required_capabilities) {
    if (!supported_required_capability(capability, snapshot != nullptr)) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["required_capability"] = capability;
      return result;
    }
  }
  if (query.timeout_budget_ns && *query.timeout_budget_ns <= 0) {
    finish_with_duration();
    record_timeout_budget(result, query);
    result.engine_status = core::CollisionEngineStatus::kTimeout;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    return result;
  }

  try {
    const auto prepared_scene = active_prepared_scene_;
    std::string q_error;
    auto q_opt = full_configuration_from_query(query, q_error);
    if (!q_opt) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["robot_state_error"] = q_error;
      return result;
    }
    const Eigen::VectorXd& q = *q_opt;

    if (snapshot != nullptr) {
      if (!prepared_scene || !active_collision_data_) {
        finish_with_duration();
        result.engine_status =
            core::CollisionEngineStatus::kDiagnosticIncomplete;
        result.decision = core::CollisionDecisionResult::kInconclusiveFail;
        result.raw_diagnostics["snapshot_sync_error"] =
            "snapshot collision model is not initialized";
        return result;
      }
      const auto pair_count =
          prepared_scene->collision_model.collisionPairs.size();
      if (prepared_scene->runtime_pair_bindings.size() != pair_count ||
          active_collision_data_->collisionResults.size() != pair_count ||
          active_collision_data_->activeCollisionPairs.size() != pair_count ||
          (distance_request_mode !=
               core::CollisionDistanceRequestMode::kNone &&
           (active_collision_data_->distanceResults.size() != pair_count ||
            active_collision_data_->distanceRequests.size() != pair_count))) {
        finish_with_duration();
        result.engine_status =
            core::CollisionEngineStatus::kDiagnosticIncomplete;
        result.decision = core::CollisionDecisionResult::kInconclusiveFail;
        result.raw_diagnostics["snapshot_sync_error"] =
            "snapshot collision data is inconsistent with the prepared model";
        return result;
      }
      result.in_collision =
          pinocchio::computeCollisions(shared_model_->model,
                                       *reusable_model_data_,
                                       prepared_scene->collision_model,
                                       *active_collision_data_,
                                       q,
                                       /* stop_at_first_collision */ false);
      last_query_mechanism_counters_.collision_api_call_count = 1;
      last_query_mechanism_counters_
          .collision_enumeration_completion_ordinal = 1;
      last_query_mechanism_counters_.fk_placement_invocation_count = 1;
    } else {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["snapshot_required"] =
          "ManyForgeCollisionEngine requires CollisionWorldSnapshot";
      return result;
    }
    if (cancel != nullptr && cancel->is_cancel_requested()) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kCancelled;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["cancel_reason"] = cancel->reason();
      return result;
    }

    const auto pair_count =
        prepared_scene->collision_model.collisionPairs.size();
    auto raw_pair_description = [&](std::size_t pair_index) {
      if (pair_index >= prepared_scene->collision_model.collisionPairs.size()) {
        return std::string("invalid geometry pair index");
      }
      const auto& geometry_pair =
          prepared_scene->collision_model.collisionPairs[pair_index];
      if (geometry_pair.first >=
              prepared_scene->collision_model.geometryObjects.size() ||
          geometry_pair.second >=
              prepared_scene->collision_model.geometryObjects.size()) {
        return std::string("invalid geometry object index");
      }
      return prepared_scene->collision_model
                 .geometryObjects[geometry_pair.first]
                 .name +
             " <-> " +
             prepared_scene->collision_model
                 .geometryObjects[geometry_pair.second]
                 .name;
    };

    bool any_unknown_collision = false;
    for (std::size_t pair_index = 0; pair_index < pair_count; ++pair_index) {
      if (!active_collision_data_->activeCollisionPairs[pair_index]) {
        continue;
      }
      ++last_query_mechanism_counters_.active_collision_pair_count;
      if (!active_collision_data_->collisionResults[pair_index].isCollision()) {
        continue;
      }
      ++last_query_mechanism_counters_.colliding_geometry_pair_count;
      const auto& binding =
          prepared_scene->runtime_pair_bindings[pair_index];
      if (!binding.mapped) {
        any_unknown_collision = true;
        result.raw_diagnostics["unknown_collision_pairs"] +=
            raw_pair_description(pair_index) + ";";
        continue;
      }
      result.normalized_pairs.push_back(binding.pair);
    }
    std::sort(result.normalized_pairs.begin(), result.normalized_pairs.end());
    result.normalized_pairs.erase(
        std::unique(result.normalized_pairs.begin(),
                    result.normalized_pairs.end()),
        result.normalized_pairs.end());
    last_query_mechanism_counters_.colliding_logical_pair_count =
        result.normalized_pairs.size();
    if (result.in_collision &&
        last_query_mechanism_counters_.colliding_geometry_pair_count == 0) {
      any_unknown_collision = true;
      result.raw_diagnostics["collision_diagnostics_available"] = "false";
    }
    if (!result.normalized_pairs.empty()) {
      result.contact_body_a = result.normalized_pairs.front().body_a;
      result.contact_body_b = result.normalized_pairs.front().body_b;
    }
    if (any_unknown_collision) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      return result;
    }

    std::map<core::CollisionPairId, double> min_clearance_by_pair;
    std::map<core::CollisionPairId,
             detail::SelectedCollidingDistanceEvidence>
        selected_distance_evidence_by_pair;
    bool incomplete_distance_evidence = false;
    if (distance_request_mode ==
        core::CollisionDistanceRequestMode::kAllActivePairs) {
      last_query_mechanism_counters_.first_distance_evaluation_ordinal = 2;
      for (std::size_t pair_index = 0; pair_index < pair_count; ++pair_index) {
        if (active_collision_data_->activeCollisionPairs[pair_index]) {
          reset_distance_warm_start(
              active_collision_data_->distanceRequests[pair_index]);
          active_collision_data_->distanceRequests[pair_index]
              .enable_signed_distance = true;
        }
      }
      pinocchio::computeDistances(shared_model_->model,
                                  *reusable_model_data_,
                                  prepared_scene->collision_model,
                                  *active_collision_data_, q);
      last_query_mechanism_counters_.distance_api_call_count = 1;
      ++last_query_mechanism_counters_.fk_placement_invocation_count;
      std::set<core::CollisionPairId> evaluated_logical_pairs;
      for (std::size_t pair_index = 0; pair_index < pair_count; ++pair_index) {
        if (!active_collision_data_->activeCollisionPairs[pair_index]) {
          continue;
        }
        ++last_query_mechanism_counters_
              .distance_evaluated_geometry_pair_count;
        const auto& binding =
            prepared_scene->runtime_pair_bindings[pair_index];
        if (!binding.mapped) {
          incomplete_distance_evidence = true;
          result.raw_diagnostics["unknown_distance_pairs"] +=
              raw_pair_description(pair_index) + ";";
          continue;
        }
        evaluated_logical_pairs.insert(binding.pair);
        const double distance =
            active_collision_data_->distanceResults[pair_index].min_distance;
        if (!std::isfinite(distance)) {
          result.raw_diagnostics["nonfinite_distance_pairs"] +=
              raw_pair_description(pair_index) + ";";
          continue;
        }
        auto [it, inserted] =
            min_clearance_by_pair.emplace(binding.pair, distance);
        if (!inserted && distance < it->second) {
          it->second = distance;
        }
      }
      last_query_mechanism_counters_.distance_evaluated_logical_pair_count =
          evaluated_logical_pairs.size();
    } else if (
        distance_request_mode ==
            core::CollisionDistanceRequestMode::kAllCollidingPairs ||
        distance_request_mode ==
            core::CollisionDistanceRequestMode::kSelectedCollidingPairs) {
      for (std::size_t pair_index = 0; pair_index < pair_count; ++pair_index) {
        if (!active_collision_data_->activeCollisionPairs[pair_index] ||
            !active_collision_data_->collisionResults[pair_index]
                 .isCollision()) {
          continue;
        }
        const auto& binding =
            prepared_scene->runtime_pair_bindings[pair_index];
        const bool selected =
            distance_request_mode ==
                core::CollisionDistanceRequestMode::kAllCollidingPairs ||
            pair_is_selected(
                binding.pair,
                std::span<const core::CollisionPairId>(
                    query.selected_distance_pairs));
        if (!selected) {
          continue;
        }
        if (cancel != nullptr && cancel->is_cancel_requested()) {
          finish_with_duration();
          result.engine_status = core::CollisionEngineStatus::kCancelled;
          result.decision = core::CollisionDecisionResult::kInconclusiveFail;
          result.raw_diagnostics["cancel_reason"] = cancel->reason();
          return result;
        }
        if (last_query_mechanism_counters_.distance_api_call_count == 0) {
          last_query_mechanism_counters_.first_distance_evaluation_ordinal = 2;
        }
        reset_distance_warm_start(
            active_collision_data_->distanceRequests[pair_index]);
        active_collision_data_->distanceRequests[pair_index]
            .enable_signed_distance = true;
        auto& distance_result = pinocchio::computeDistance(
            prepared_scene->collision_model, *active_collision_data_,
            pair_index);
        ++last_query_mechanism_counters_.distance_api_call_count;
        ++last_query_mechanism_counters_
              .distance_evaluated_geometry_pair_count;
        const double distance = distance_result.min_distance;
        auto& selected_evidence =
            selected_distance_evidence_by_pair[binding.pair];
        selected_evidence.observe(distance);
        if (!std::isfinite(distance)) {
          incomplete_distance_evidence = true;
          result.raw_diagnostics["nonfinite_distance_pairs"] +=
              raw_pair_description(pair_index) + ";";
          continue;
        }
      }
      for (const auto& [pair, evidence] :
           selected_distance_evidence_by_pair) {
        if (evidence.complete && evidence.deepest_finite_m) {
          min_clearance_by_pair.emplace(pair, *evidence.deepest_finite_m);
        }
      }
      for (const auto& pair : result.normalized_pairs) {
        const bool selected =
            distance_request_mode ==
                core::CollisionDistanceRequestMode::kAllCollidingPairs ||
            pair_is_selected(
                pair, std::span<const core::CollisionPairId>(
                          query.selected_distance_pairs));
        if (!selected) {
          continue;
        }
        ++last_query_mechanism_counters_
              .distance_evaluated_logical_pair_count;
        const auto aggregate =
            selected_distance_evidence_by_pair.find(pair);
        if (aggregate != selected_distance_evidence_by_pair.end() &&
            !aggregate->second.complete) {
          continue;
        }
        const auto evidence = min_clearance_by_pair.find(pair);
        if (evidence == min_clearance_by_pair.end()) {
          incomplete_distance_evidence = true;
          result.raw_diagnostics["missing_selected_distance_pairs"] +=
              pair.body_a + " <-> " + pair.body_b + ";";
        } else if (evidence->second > 0.0) {
          incomplete_distance_evidence = true;
          result.raw_diagnostics["contradictory_selected_distance_pairs"] +=
              pair.body_a + " <-> " + pair.body_b + ";";
        }
      }
    }

    for (const auto& [pair, distance] : min_clearance_by_pair) {
      result.pair_clearances.push_back(core::CollisionPairClearance{
          pair,
          distance,
      });
      if (!result.min_clearance_m || distance < *result.min_clearance_m) {
        result.min_clearance_m = distance;
        result.min_clearance_pair = pair;
      }
    }
    if (cancel != nullptr && cancel->is_cancel_requested()) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kCancelled;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["cancel_reason"] = cancel->reason();
      return result;
    }
    if (incomplete_distance_evidence) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      return result;
    }
  } catch (const std::exception& exc) {
    finish_with_duration();
    result.engine_status = core::CollisionEngineStatus::kEngineError;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    result.raw_diagnostics["engine_exception"] = exc.what();
    return result;
  } catch (...) {
    finish_with_duration();
    result.engine_status = core::CollisionEngineStatus::kEngineError;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    return result;
  }
  finish_with_duration();
  if (query.timeout_budget_ns &&
      result.query_duration_ns &&
      *result.query_duration_ns > *query.timeout_budget_ns) {
    record_timeout_exceeded(result, query, /*includes_snapshot_sync=*/false);
    result.engine_status = core::CollisionEngineStatus::kTimeout;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    return result;
  }
  result.engine_status = core::CollisionEngineStatus::kOk;
  result.decision = result.in_collision ? core::CollisionDecisionResult::kFail
                                         : core::CollisionDecisionResult::kPass;
  return result;
}

std::optional<Eigen::VectorXd>
ManyForgeCollisionEngine::full_configuration_from_query(
    const core::CollisionQuery& query,
    std::string& error) const {
  const auto& positions = query.robot_state.positions_rad;
  if (positions.empty()) {
    error = "robot_state.positions_rad is empty";
    return std::nullopt;
  }
  if (static_cast<Eigen::Index>(positions.size()) == shared_model_->model.nq) {
    Eigen::VectorXd q(shared_model_->model.nq);
    for (std::size_t i = 0; i < positions.size(); ++i) {
      q(static_cast<Eigen::Index>(i)) = positions[i];
    }
    return q;
  }

  Eigen::VectorXd q = pinocchio::neutral(shared_model_->model);
  auto fill_joint = [&](const std::string& joint_name,
                        double value) -> bool {
    if (!shared_model_->model.existJointName(joint_name)) {
      error = "joint not present in model: " + joint_name;
      return false;
    }
    const auto joint_id = shared_model_->model.getJointId(joint_name);
    if (joint_id >= shared_model_->model.idx_qs.size() ||
        joint_id >= shared_model_->model.nqs.size()) {
      error = "joint index out of range for: " + joint_name;
      return false;
    }
    if (shared_model_->model.nqs[joint_id] != 1) {
      error = "joint is not scalar in current primitive profile: " + joint_name;
      return false;
    }
    q(shared_model_->model.idx_qs[joint_id]) = value;
    return true;
  };

  if (query.robot_state.joint_names.size() == positions.size()) {
    for (std::size_t i = 0; i < positions.size(); ++i) {
      if (!fill_joint(query.robot_state.joint_names[i], positions[i])) {
        return std::nullopt;
      }
    }
    return q;
  }

  if (shared_model_->group_joint_names.size() == positions.size()) {
    for (std::size_t i = 0; i < positions.size(); ++i) {
      if (!fill_joint(shared_model_->group_joint_names[i], positions[i])) {
        return std::nullopt;
      }
    }
    return q;
  }

  error = "robot_state.positions_rad size " + std::to_string(positions.size()) +
          " does not match model.nq " + std::to_string(shared_model_->model.nq) +
          ", robot_state.joint_names, or SRDF group '" + shared_model_->group_name + "'";
  return std::nullopt;
}

std::optional<std::string> ManyForgeCollisionEngine::sync_snapshot_locked(
    const core::CollisionWorldSnapshot& snapshot, bool allow_resource_io) {
  const auto revision_matches = [&](const std::shared_ptr<const PreparedScene>& prepared) {
    return prepared &&
           prepared->revision.revision_id == snapshot.revision.revision_id &&
           prepared->revision.content_hash == snapshot.revision.content_hash;
  };
  const auto bind_prepared_scene = [&](std::shared_ptr<const PreparedScene> prepared) {
    active_prepared_scene_ = std::move(prepared);
    active_collision_data_.emplace(active_prepared_scene_->collision_model);
    runtime_collision_observations_.clear();
    runtime_clearance_observations_.clear();
    const auto pair_count =
        active_prepared_scene_->collision_model.collisionPairs.size();
    runtime_collision_observations_.reserve(pair_count);
    runtime_clearance_observations_.reserve(pair_count);
  };

  if (revision_matches(active_prepared_scene_)) {
    return std::nullopt;
  }
  if (auto cached = scene_cache_->latest.load(std::memory_order_acquire);
      revision_matches(cached)) {
    bind_prepared_scene(std::move(cached));
    return std::nullopt;
  }

  active_prepared_scene_.reset();
  active_collision_data_.reset();
  runtime_collision_observations_.clear();
  runtime_clearance_observations_.clear();
  auto prepared = std::make_shared<PreparedScene>();
  prepared->revision = snapshot.revision;

  std::map<std::string, std::map<int, core::CollisionBodyId>>
      robot_ids_by_link_index;
  for (const auto& body : snapshot.bodies) {
    if (body.owner_kind != core::CollisionOwnerKind::kRobotLink) {
      continue;
    }
    auto link = robot_link_from_collision_body_id(body.body_id);
    if (!link) {
      continue;
    }
    const auto suffix = body.body_id.rfind("/collision/");
    int collision_index = 0;
    if (suffix != std::string::npos) {
      collision_index = std::stoi(body.body_id.substr(suffix + 11));
    }
    robot_ids_by_link_index[*link][collision_index] = body.body_id;
  }

  std::map<std::string, core::CollisionBodyId> builtin_raw_to_body_id;
  std::map<std::string, int> next_collision_index_by_link;
  for (const auto& geometry_object : shared_model_->base_collision_model.geometryObjects) {
    if (geometry_object.parentFrame >= shared_model_->model.frames.size()) {
      continue;
    }
    const auto link_name = shared_model_->model.frames[geometry_object.parentFrame].name;
    const int collision_index = next_collision_index_by_link[link_name]++;
    auto link_it = robot_ids_by_link_index.find(link_name);
    if (link_it == robot_ids_by_link_index.end()) {
      continue;
    }
    auto body_it = link_it->second.find(collision_index);
    if (body_it == link_it->second.end()) {
      continue;
    }
    builtin_raw_to_body_id[geometry_object.name] = body_it->second;
  }

  std::set<std::pair<std::string, std::string>> active_robot_body_pairs;
  for (const auto& pair : shared_model_->base_collision_model.collisionPairs) {
    if (pair.first >= shared_model_->base_collision_model.geometryObjects.size() ||
        pair.second >= shared_model_->base_collision_model.geometryObjects.size()) {
      continue;
    }
    auto a = builtin_raw_to_body_id.find(
        shared_model_->base_collision_model.geometryObjects[pair.first].name);
    auto b = builtin_raw_to_body_id.find(
        shared_model_->base_collision_model.geometryObjects[pair.second].name);
    if (a == builtin_raw_to_body_id.end() ||
        b == builtin_raw_to_body_id.end()) {
      continue;
    }
    active_robot_body_pairs.insert(canonical_pair(a->second, b->second));
  }

  pinocchio::SE3 model_from_world = pinocchio::SE3::Identity();
  if (snapshot.robot_model_frames.size() > 1) {
    return "manyforge_collision currently supports one robot_model_frame per snapshot";
  }
  if (!snapshot.robot_model_frames.empty()) {
    const auto& frame = snapshot.robot_model_frames.front();
    if (!shared_model_->model.existFrame(frame.model_frame_id)) {
      return "robot_model_frame model_frame_id not present in engine model: " +
             frame.model_frame_id;
    }
    pinocchio::Data neutral_data(shared_model_->model);
    const Eigen::VectorXd q_neutral = pinocchio::neutral(shared_model_->model);
    pinocchio::framesForwardKinematics(shared_model_->model, neutral_data, q_neutral);
    const auto frame_id = shared_model_->model.getFrameId(frame.model_frame_id);
    if (frame_id >= neutral_data.oMf.size()) {
      return "robot_model_frame frame index out of range: " +
             frame.model_frame_id;
    }
    const pinocchio::SE3 model_from_frame = neutral_data.oMf[frame_id];
    const pinocchio::SE3 world_from_frame = pose_to_se3(frame.world_pose);
    const pinocchio::SE3 world_from_model =
        world_from_frame * model_from_frame.inverse();
    model_from_world = world_from_model.inverse();
  }

  std::size_t index = 0;
  for (const auto& body : snapshot.bodies) {
    if (body.geometries.empty()) {
      return "manyforge_collision requires at least one geometry per body: " +
             body.body_id;
    }
    std::string target_frame;
    if (body.owner_kind == core::CollisionOwnerKind::kRobotLink) {
      target_frame = robot_link_from_collision_body_id(body.body_id).value_or("");
    } else if (body.owner_kind == core::CollisionOwnerKind::kAttachedObject) {
      target_frame =
          link_name_from_parent_body_id(body.parent_link.value_or(""))
              .value_or("");
    } else {
      target_frame = "universe";
    }
    if (target_frame.empty()) {
      return "body has invalid target frame for manyforge_collision: " +
             body.body_id;
    }

    for (const auto& geom : body.geometries) {
      const std::string raw_name =
          sanitized_geometry_name(body.body_id, index++);
      std::shared_ptr<coal::CollisionGeometry> shape;
      switch (geom.geometry_kind) {
        case core::CollisionGeometryKind::kBox: {
          if (!geom.box_dimensions_m) {
            return "box geometry missing dimensions: " + body.body_id;
          }
          shape = std::make_shared<coal::Box>(
              geom.box_dimensions_m->x_m, geom.box_dimensions_m->y_m,
              geom.box_dimensions_m->z_m);
          break;
        }
        case core::CollisionGeometryKind::kSphere: {
          if (!geom.sphere_radius_m) {
            return "sphere geometry missing radius: " + body.body_id;
          }
          shape = std::make_shared<coal::Sphere>(*geom.sphere_radius_m);
          break;
        }
        case core::CollisionGeometryKind::kCylinder: {
          if (!geom.cylinder_radius_m || !geom.cylinder_height_m) {
            return "cylinder geometry missing dimensions: " + body.body_id;
          }
          shape = std::make_shared<coal::Cylinder>(
              *geom.cylinder_radius_m, *geom.cylinder_height_m);
          break;
        }
        case core::CollisionGeometryKind::kTriangleMesh: {
          if (!geom.source_uri || geom.source_uri->empty()) {
            return "triangle mesh geometry missing source_uri: " +
                   body.body_id;
          }
          const Eigen::Vector3d scale = effective_mesh_scale(geom.mesh_scale);
          if (!(scale.array().isFinite()).all() ||
              !(scale.array() > 0.0).all()) {
            return "triangle mesh geometry has invalid mesh_scale: " +
                   body.body_id;
          }
          const auto cache_key = mesh_cache_key(*geom.source_uri, scale);
          {
            std::lock_guard<std::mutex> cache_lock(
                scene_cache_->mesh_cache_mutex);
            auto cached = scene_cache_->mesh_geometry_cache.find(cache_key);
            if (cached != scene_cache_->mesh_geometry_cache.end()) {
              shape = cached->second;
            } else {
              if (!allow_resource_io) {
                return "runtime snapshot references an unprepared triangle "
                       "mesh: " +
                       body.body_id;
              }
              if (scene_cache_->mesh_geometry_cache.size() >=
                  capability_.runtime_query_max_world_bodies) {
                return "prepared triangle-mesh cache exceeds qualified body "
                       "bound";
              }
              std::string mesh_error;
              auto mesh_path = resolve_mesh_resource_uri(
                  *geom.source_uri, shared_model_->package_paths, mesh_error);
              if (!mesh_path) {
                return "triangle mesh geometry could not resolve source_uri "
                       "for " +
                       body.body_id + ": " + mesh_error;
              }
              coal::MeshLoader loader;
              auto mesh = loader.load(mesh_path->string(), scale);
              if (!mesh) {
                return "triangle mesh loader returned null for " +
                       body.body_id + ": " + mesh_path->string();
              }
              shape = std::move(mesh);
              scene_cache_->mesh_geometry_cache[cache_key] = shape;
            }
          }
          break;
        }
        case core::CollisionGeometryKind::kCapsule:
        case core::CollisionGeometryKind::kConvexMesh:
        case core::CollisionGeometryKind::kCompound:
          return "unsupported scene geometry kind for manyforge_collision: " +
                 body.body_id;
      }

      std::optional<pinocchio::GeometryObject> object;
      if (target_frame != "universe") {
        if (!shared_model_->model.existFrame(target_frame)) {
          return "target frame not present in model: " + target_frame +
                 " for body " + body.body_id;
        }
        const auto frame_id = shared_model_->model.getFrameId(target_frame);
        const auto& frame = shared_model_->model.frames[frame_id];
        const auto frame_relative_placement =
            body.owner_kind == core::CollisionOwnerKind::kRobotLink
                ? pose_to_se3(geom.local_pose)
                : compose_pose_to_se3(body.pose, geom.local_pose);
        const auto placement = frame.placement * frame_relative_placement;
        object.emplace(raw_name, frame.parentJoint, frame_id, placement, shape);
      } else {
        const pinocchio::SE3 body_geometry_in_model =
            model_from_world * compose_pose_to_se3(body.pose, geom.local_pose);
        object.emplace(raw_name, pinocchio::JointIndex(0),
                       pinocchio::FrameIndex(0), body_geometry_in_model, shape);
      }
      prepared->collision_model.addGeometryObject(*object,
                                                  shared_model_->model);
      prepared->scene_geometry_names.push_back(raw_name);
      prepared->raw_name_to_body_id[raw_name] = body.body_id;
    }
  }

  std::map<std::string, std::size_t> synced_index_by_name;
  for (std::size_t i = 0; i < prepared->collision_model.geometryObjects.size();
       ++i) {
    const auto& name = prepared->collision_model.geometryObjects[i].name;
    if (prepared->raw_name_to_body_id.count(name) > 0) {
      synced_index_by_name[name] = i;
    }
  }
  std::set<std::pair<std::size_t, std::size_t>> active_pair_indices;
  for (const auto& pair : prepared->collision_model.collisionPairs) {
    const auto first = std::min(pair.first, pair.second);
    const auto second = std::max(pair.first, pair.second);
    active_pair_indices.insert({first, second});
  }
  for (auto it_a = synced_index_by_name.begin();
       it_a != synced_index_by_name.end(); ++it_a) {
    auto body_a = prepared->raw_name_to_body_id.find(it_a->first);
    if (body_a == prepared->raw_name_to_body_id.end()) {
      continue;
    }
    auto it_b = it_a;
    ++it_b;
    for (; it_b != synced_index_by_name.end(); ++it_b) {
      auto body_b = prepared->raw_name_to_body_id.find(it_b->first);
      if (body_b == prepared->raw_name_to_body_id.end()) {
        continue;
      }
      if (body_a->second == body_b->second) {
        // Geometry elements belonging to one logical collision body are a
        // compound shape, not a policy-visible self-collision pair.
        continue;
      }
      const bool a_motion_relevant =
          body_is_motion_relevant(body_a->second, shared_model_->model, shared_model_->group_joint_names);
      const bool b_motion_relevant =
          body_is_motion_relevant(body_b->second, shared_model_->model, shared_model_->group_joint_names);
      if (!a_motion_relevant && !b_motion_relevant) {
        continue;
      }

      const bool a_robot_link = is_robot_link_body_id(body_a->second);
      const bool b_robot_link = is_robot_link_body_id(body_b->second);
      if (a_robot_link && b_robot_link &&
          active_robot_body_pairs.count(
              canonical_pair(body_a->second, body_b->second)) == 0) {
        continue;
      }
      const auto first = std::min(it_a->second, it_b->second);
      const auto second = std::max(it_a->second, it_b->second);
      if (active_pair_indices.find({first, second}) ==
          active_pair_indices.end()) {
        if (active_pair_indices.size() >=
            capability_.runtime_query_max_collision_pairs) {
          return "runtime collision-pair count exceeds qualified bound";
        }
        active_pair_indices.insert({first, second});
        prepared->collision_model.addCollisionPair(
            pinocchio::CollisionPair(first, second));
      }
    }
  }

  // Revision-stable: every input to this map was just rebuilt above, so
  // build it once here instead of re-parsing body ids on every check.
  prepared->raw_to_body_map = build_raw_to_body_map(
      shared_model_->model, prepared->collision_model, snapshot,
      prepared->scene_geometry_names, prepared->raw_name_to_body_id);

  const auto pair_count = prepared->collision_model.collisionPairs.size();
  if (pair_count > capability_.runtime_query_max_collision_pairs) {
    return "runtime collision-pair count exceeds qualified bound";
  }
  prepared->runtime_pair_bindings.resize(pair_count);
  for (std::size_t i = 0; i < pair_count; ++i) {
    const auto& pair = prepared->collision_model.collisionPairs[i];
    if (pair.first >= prepared->collision_model.geometryObjects.size() ||
        pair.second >= prepared->collision_model.geometryObjects.size()) {
      continue;
    }
    const auto& raw_a =
        prepared->collision_model.geometryObjects[pair.first].name;
    const auto& raw_b =
        prepared->collision_model.geometryObjects[pair.second].name;
    const auto body_a = prepared->raw_to_body_map.find(raw_a);
    const auto body_b = prepared->raw_to_body_map.find(raw_b);
    if (body_a == prepared->raw_to_body_map.end() ||
        body_b == prepared->raw_to_body_map.end()) {
      continue;
    }
    auto& binding = prepared->runtime_pair_bindings[i];
    binding.mapped = true;
    binding.pair = {body_a->second, body_b->second};
    if (binding.pair.body_b < binding.pair.body_a) {
      std::swap(binding.pair.body_a, binding.pair.body_b);
    }
  }

  // Publish immutable preparation with monotonic revision ownership. Parallel
  // cold misses may build equivalent candidates, but only one becomes the
  // shared value. Publication itself is lock-free; mesh-resource reuse remains
  // serialized only inside this NRT cache domain.
  std::shared_ptr<const PreparedScene> selected = prepared;
  auto observed = scene_cache_->latest.load(std::memory_order_acquire);
  for (;;) {
    if (revision_matches(observed)) {
      selected = std::move(observed);
      break;
    }
    if (observed &&
        observed->revision.revision_id == snapshot.revision.revision_id &&
        observed->revision.content_hash != snapshot.revision.content_hash) {
      return "collision snapshot revision reused with different content hash";
    }
    if (observed &&
        observed->revision.revision_id > snapshot.revision.revision_id) {
      // An older in-flight validation may finish after a newer publication.
      // Use its prepared value locally without regressing the shared cache.
      break;
    }
    if (scene_cache_->latest.compare_exchange_weak(
            observed, prepared, std::memory_order_release,
            std::memory_order_acquire)) {
      scene_cache_->build_count.fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }

  bind_prepared_scene(std::move(selected));
  return std::nullopt;
}

}  // namespace manyforge::collision
