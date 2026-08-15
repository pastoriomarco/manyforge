#include <manyforge_collision/manyforge_collision_engine.hpp>

#include <Eigen/Dense>
#include <algorithm>
#include <atomic>
#include <charconv>
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
#include <tuple>
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
#include <coal/shape/convex.h>
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

std::optional<std::size_t> component_body_collision_index(
    const std::string& body_id) {
  constexpr std::string_view kPrefix = "component/";
  constexpr std::string_view kModel = "/model/";
  constexpr std::string_view kBody = "/body/";
  constexpr std::string_view kCollision = "/collision/";
  if (body_id.rfind(std::string(kPrefix), 0) != 0) {
    return std::nullopt;
  }
  const auto model_pos = body_id.find(std::string(kModel), kPrefix.size());
  const auto body_pos = model_pos == std::string::npos
                            ? std::string::npos
                            : body_id.find(std::string(kBody),
                                           model_pos + kModel.size());
  const auto collision_pos = body_pos == std::string::npos
                                 ? std::string::npos
                                 : body_id.find(std::string(kCollision),
                                                body_pos + kBody.size());
  if (model_pos == std::string::npos || body_pos == std::string::npos ||
      collision_pos == std::string::npos ||
      body_id.find('/', collision_pos + kCollision.size()) !=
          std::string::npos) {
    return std::nullopt;
  }
  const auto nonempty_segment = [&](std::size_t begin, std::size_t end) {
    return begin < end;
  };
  if (!nonempty_segment(kPrefix.size(), model_pos) ||
      !nonempty_segment(model_pos + kModel.size(), body_pos) ||
      !nonempty_segment(body_pos + kBody.size(), collision_pos)) {
    return std::nullopt;
  }
  const std::string_view index_text(body_id.data() + collision_pos +
                                        kCollision.size(),
                                    body_id.size() - collision_pos -
                                        kCollision.size());
  if (index_text.empty() ||
      (index_text.size() > 1U && index_text.front() == '0')) {
    return std::nullopt;
  }
  std::size_t index = 0;
  const auto parsed = std::from_chars(index_text.data(),
                                      index_text.data() + index_text.size(),
                                      index);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != index_text.data() + index_text.size()) {
    return std::nullopt;
  }
  return index;
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
  if (candidate.is_relative()) {
    for (const auto& prefix : package_paths) {
      candidates.push_back(prefix / candidate);
    }
    candidates.push_back(candidate);
    candidates.push_back(std::filesystem::current_path() / candidate);
    candidates.push_back(std::filesystem::path("/workspace") / candidate);
  } else {
    candidates.push_back(candidate);
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

core::Pose3DTO pose_from_se3(const pinocchio::SE3& pose) {
  const Eigen::Quaterniond orientation(pose.rotation());
  return {
      {pose.translation().x(), pose.translation().y(), pose.translation().z()},
      {orientation.x(), orientation.y(), orientation.z(), orientation.w()},
  };
}

core::Result<RetainedComponentCollisionProjection> projection_error(
    std::string message) {
  return core::Result<RetainedComponentCollisionProjection>::Err(
      core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
          std::move(message),
          {},
      });
}

core::Result<RetainedCarriedObjectParentProjection>
carried_parent_projection_error(std::string message) {
  return core::Result<RetainedCarriedObjectParentProjection>::Err(
      core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
          std::move(message),
          {},
      });
}

std::string component_collision_body_id(
    const RetainedComponentCollisionBodyProjectionInput& input) {
  return "component/" + input.controllable_component_id.value() +
         "/model/" + input.model_instance_id.value() + "/body/" +
         input.collision_body_role_id.value() + "/collision/" +
         std::to_string(input.collision_index);
}

void add_quality_label(core::CollisionGeometryDescriptor& descriptor,
                       std::string label) {
  descriptor.quality_labels.push_back(std::move(label));
  std::sort(descriptor.quality_labels.begin(),
            descriptor.quality_labels.end());
  descriptor.quality_labels.erase(
      std::unique(descriptor.quality_labels.begin(),
                  descriptor.quality_labels.end()),
      descriptor.quality_labels.end());
}

core::Vector3DTO vector3_from_eigen(const Eigen::Vector3d& value) {
  return {value.x(), value.y(), value.z()};
}

core::CollisionGeometryDescriptor retained_native_geometry_descriptor(
    const core::CollisionBodyId& body_id,
    const RetainedComponentCollisionBodyProjectionInput& input,
    const pinocchio::Model& model,
    const pinocchio::GeometryObject& geometry_object) {
  if (geometry_object.parentFrame >= model.frames.size()) {
    throw std::invalid_argument(
        "retained native geometry has an invalid parent frame");
  }
  const auto& frame = model.frames[geometry_object.parentFrame];
  const pinocchio::SE3 frame_from_geometry =
      frame.placement.inverse() * geometry_object.placement;

  core::CollisionGeometryDescriptor descriptor;
  descriptor.body_id = body_id;
  descriptor.owner_kind = core::CollisionOwnerKind::kComponentModelBody;
  descriptor.local_pose = pose_from_se3(frame_from_geometry);
  descriptor.quality_labels = {"exact_retained_native_geometry"};

  const auto& geometry = geometry_object.geometry;
  if (geometry != nullptr) {
    if (const auto* box = dynamic_cast<const coal::Box*>(geometry.get());
        box != nullptr) {
      descriptor.geometry_kind = core::CollisionGeometryKind::kBox;
      descriptor.box_dimensions_m = vector3_from_eigen(2.0 * box->halfSide);
      add_quality_label(descriptor, "primitive");
      return descriptor;
    }
    if (const auto* sphere =
            dynamic_cast<const coal::Sphere*>(geometry.get());
        sphere != nullptr) {
      descriptor.geometry_kind = core::CollisionGeometryKind::kSphere;
      descriptor.sphere_radius_m = sphere->radius;
      add_quality_label(descriptor, "primitive");
      return descriptor;
    }
    if (const auto* cylinder =
            dynamic_cast<const coal::Cylinder*>(geometry.get());
        cylinder != nullptr) {
      descriptor.geometry_kind = core::CollisionGeometryKind::kCylinder;
      descriptor.cylinder_radius_m = cylinder->radius;
      descriptor.cylinder_height_m = 2.0 * cylinder->halfLength;
      add_quality_label(descriptor, "primitive");
      return descriptor;
    }
    if (const auto* capsule =
            dynamic_cast<const coal::Capsule*>(geometry.get());
        capsule != nullptr) {
      descriptor.geometry_kind = core::CollisionGeometryKind::kCapsule;
      descriptor.capsule_radius_m = capsule->radius;
      descriptor.capsule_height_m = 2.0 * capsule->halfLength;
      add_quality_label(descriptor, "primitive");
      return descriptor;
    }
  }

  const std::string generated_source =
      "retained-native://component/" +
      input.controllable_component_id.value() + "/model/" +
      input.model_instance_id.value() + "/frame/" +
      input.compiled_frame_name + "/geometry/" + geometry_object.name;
  descriptor.source_uri = generated_source;
  descriptor.source_hash = input.model_revision.value();
  if (geometry != nullptr &&
      dynamic_cast<const coal::ConvexBase*>(geometry.get()) != nullptr) {
    descriptor.geometry_kind = core::CollisionGeometryKind::kConvexMesh;
    descriptor.mesh_scale =
        vector3_from_eigen(geometry_object.meshScale);
    add_quality_label(descriptor, "convex");
  } else if (geometry != nullptr &&
             dynamic_cast<const coal::BVHModelBase*>(geometry.get()) !=
                 nullptr) {
    descriptor.geometry_kind = core::CollisionGeometryKind::kTriangleMesh;
    descriptor.mesh_scale =
        vector3_from_eigen(geometry_object.meshScale);
    add_quality_label(descriptor, "collision_mesh");
  } else {
    descriptor.geometry_kind = core::CollisionGeometryKind::kCompound;
    add_quality_label(descriptor, "backend_compound");
  }
  return descriptor;
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

core::Result<core::Pose3DTO> ManyForgeCollisionEngine::resolve_retained_frame_pose(
    SharedModelPtr shared_model, const std::string& compiled_cell_frame_name,
    const std::string& compiled_target_frame_name,
    const std::span<const RetainedKinematicCoordinateValue> coordinates) {
  const auto fail = [](std::string message) {
    return core::Result<core::Pose3DTO>::Err(
        core::ErrorDTO{std::string(core::to_string(core::ErrorCode::kMissingReference)),
                       std::move(message),
                       {{"reason", "retained_frame_resolution_failed"}}});
  };
  if (!shared_model || compiled_cell_frame_name.empty() || compiled_target_frame_name.empty() ||
      !shared_model->model.existFrame(compiled_cell_frame_name) ||
      !shared_model->model.existFrame(compiled_target_frame_name)) {
    return fail("retained frame resolution names an unknown model frame");
  }

  Eigen::VectorXd q = pinocchio::neutral(shared_model->model);
  std::set<pinocchio::JointIndex> assigned;
  for (const auto& coordinate : coordinates) {
    if (coordinate.compiled_joint_name.empty() || !std::isfinite(coordinate.position_si) ||
        !shared_model->model.existJointName(coordinate.compiled_joint_name)) {
      return fail("retained frame state names an invalid scalar coordinate");
    }
    const auto joint = shared_model->model.getJointId(coordinate.compiled_joint_name);
    if (joint >= shared_model->model.idx_qs.size() || joint >= shared_model->model.nqs.size() ||
        shared_model->model.nqs[joint] != 1 || !assigned.insert(joint).second) {
      return fail("retained frame state is non-scalar or duplicated");
    }
    const auto index = shared_model->model.idx_qs[joint];
    if (index < 0 || index >= q.size()) {
      return fail("retained frame state has an invalid model index");
    }
    q[index] = coordinate.position_si;
  }

  try {
    pinocchio::Data data(shared_model->model);
    pinocchio::framesForwardKinematics(shared_model->model, data, q);
    const auto cell = shared_model->model.getFrameId(compiled_cell_frame_name);
    const auto target = shared_model->model.getFrameId(compiled_target_frame_name);
    if (cell >= data.oMf.size() || target >= data.oMf.size()) {
      return fail("retained frame resolution index is out of range");
    }
    return core::Result<core::Pose3DTO>::Ok(
        pose_from_se3(data.oMf[cell].inverse() * data.oMf[target]));
  } catch (const std::exception& error) {
    return fail("retained frame FK failed: " + std::string(error.what()));
  } catch (...) {
    return fail("retained frame FK failed");
  }
}

core::Result<std::vector<double>> ManyForgeCollisionEngine::retained_neutral_scalar_joint_positions(
    SharedModelPtr shared_model, const std::span<const std::string> compiled_joint_names) {
  const auto fail = [](std::string message) {
    return core::Result<std::vector<double>>::Err(
        core::ErrorDTO{std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
                       std::move(message),
                       {{"reason", "retained_neutral_state_projection_failed"}}});
  };
  if (!shared_model) {
    return fail("retained neutral state requires a loaded model");
  }
  const Eigen::VectorXd neutral = pinocchio::neutral(shared_model->model);
  std::set<pinocchio::JointIndex> selected;
  std::vector<double> values;
  values.reserve(compiled_joint_names.size());
  for (const auto& name : compiled_joint_names) {
    if (name.empty() || !shared_model->model.existJointName(name)) {
      return fail("retained neutral state names an unknown joint");
    }
    const auto joint = shared_model->model.getJointId(name);
    if (joint >= shared_model->model.idx_qs.size() || joint >= shared_model->model.nqs.size() ||
        shared_model->model.nqs[joint] != 1 || !selected.insert(joint).second) {
      return fail("retained neutral state names a duplicate or non-scalar joint");
    }
    const auto index = shared_model->model.idx_qs[joint];
    if (index < 0 || index >= neutral.size() || !std::isfinite(neutral[index])) {
      return fail("retained neutral state has an invalid model index");
    }
    values.push_back(neutral[index]);
  }
  return core::Result<std::vector<double>>::Ok(std::move(values));
}

bool ManyForgeCollisionEngine::retained_frame_exists(
    SharedModelPtr shared_model, const std::string& compiled_frame_name) noexcept {
  return shared_model && !compiled_frame_name.empty() &&
         shared_model->model.existFrame(compiled_frame_name);
}

core::Result<std::vector<core::CollisionBodyId>>
ManyForgeCollisionEngine::select_retained_static_component_bodies(
    SharedModelPtr shared_model,
    const std::span<const RetainedComponentCollisionBodyProjectionInput> bodies,
    const std::span<const std::string> dynamic_compiled_joint_names) {
  const auto fail = [](std::string message) {
    return core::Result<std::vector<core::CollisionBodyId>>::Err(
        core::ErrorDTO{std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
                       std::move(message),
                       {{"reason", "retained_static_body_projection_failed"}}});
  };
  if (!shared_model) {
    return fail("retained static-body selection requires a loaded model");
  }
  std::set<pinocchio::JointIndex> dynamic_joints;
  for (const auto& name : dynamic_compiled_joint_names) {
    if (name.empty() || !shared_model->model.existJointName(name) ||
        !dynamic_joints.insert(shared_model->model.getJointId(name)).second) {
      return fail("retained dynamic-coordinate inventory is invalid");
    }
  }

  std::vector<core::CollisionBodyId> result;
  std::set<core::CollisionBodyId> ids;
  for (const auto& body : bodies) {
    if (body.compiled_frame_name.empty() ||
        !shared_model->model.existFrame(body.compiled_frame_name)) {
      return fail("retained component body names an unknown frame");
    }
    auto joint =
        shared_model->model.frames[shared_model->model.getFrameId(body.compiled_frame_name)]
            .parentJoint;
    bool dynamic = false;
    while (joint != 0U) {
      if (dynamic_joints.contains(joint)) {
        dynamic = true;
        break;
      }
      if (joint >= shared_model->model.parents.size() ||
          shared_model->model.parents[joint] == joint) {
        return fail("retained component-body kinematic ancestry is invalid");
      }
      joint = shared_model->model.parents[joint];
    }
    if (!dynamic) {
      core::CollisionBodyId id(component_collision_body_id(body));
      if (!ids.insert(id).second) {
        return fail("retained static component body identity is duplicated");
      }
      result.push_back(std::move(id));
    }
  }
  std::sort(result.begin(), result.end());
  return core::Result<std::vector<core::CollisionBodyId>>::Ok(std::move(result));
}

core::Result<RetainedComponentCollisionProjection>
ManyForgeCollisionEngine::project_retained_component_collision(
    SharedModelPtr shared_model,
    const std::string& compiled_cell_frame_name,
    std::span<const RetainedComponentCollisionBodyProjectionInput> bodies,
    std::span<const RetainedComponentModelFrameProjectionInput> frames) {
  if (!shared_model) {
    return projection_error(
        "retained component collision projection requires a loaded model");
  }
  if (compiled_cell_frame_name.empty() ||
      !shared_model->model.existFrame(compiled_cell_frame_name)) {
    return projection_error(
        "retained component collision projection cell frame is absent from "
        "the loaded model");
  }
  if (bodies.empty()) {
    return projection_error(
        "retained component collision projection has no component bodies");
  }

  using OccurrenceKey =
      std::tuple<core::ControllableComponentId, core::ModelInstanceId,
                 core::ModelRevision>;
  std::map<std::string, std::size_t> body_index_by_compiled_frame;
  std::set<OccurrenceKey> collision_occurrences;
  RetainedComponentCollisionProjection projection;
  projection.component_bodies.reserve(bodies.size());

  for (const auto& input : bodies) {
    if (input.controllable_component_id.empty() ||
        input.model_instance_id.empty() || input.model_revision.empty() ||
        input.collision_body_role_id.empty() ||
        input.compiled_frame_name.empty() ||
        !shared_model->model.existFrame(input.compiled_frame_name)) {
      return projection_error(
          "retained component collision body mapping is incomplete or names "
          "an unknown compiled frame");
    }
    if (!body_index_by_compiled_frame
             .emplace(input.compiled_frame_name,
                      projection.component_bodies.size())
             .second) {
      return projection_error(
          "retained collision-body roles overlap on compiled frame: " +
          input.compiled_frame_name);
    }
    collision_occurrences.emplace(input.controllable_component_id,
                                  input.model_instance_id,
                                  input.model_revision);
    core::CollisionWorldBody body;
    body.body_id = component_collision_body_id(input);
    body.owner_kind = core::CollisionOwnerKind::kComponentModelBody;
    body.frame_id = input.compiled_frame_name;
    body.pose = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
    projection.component_bodies.push_back(std::move(body));
  }

  std::set<std::string> native_geometry_names;
  for (const auto& geometry_object :
       shared_model->base_collision_model.geometryObjects) {
    if (geometry_object.parentFrame >= shared_model->model.frames.size()) {
      return projection_error(
          "retained native collision geometry has an invalid parent frame: " +
          geometry_object.name);
    }
    if (!native_geometry_names.insert(geometry_object.name).second) {
      return projection_error(
          "retained native collision geometry name is not unique: " +
          geometry_object.name);
    }
    const std::string& compiled_frame_name =
        shared_model->model.frames[geometry_object.parentFrame].name;
    const auto body_match =
        body_index_by_compiled_frame.find(compiled_frame_name);
    if (body_match == body_index_by_compiled_frame.end()) {
      return projection_error(
          "retained native geometry has no compiler-owned collision-body "
          "mapping: " +
          geometry_object.name + " (frame=" + compiled_frame_name + ")");
    }
    auto& body = projection.component_bodies[body_match->second];
    try {
      body.geometries.push_back(retained_native_geometry_descriptor(
          body.body_id, bodies[body_match->second], shared_model->model,
          geometry_object));
    } catch (const std::exception& error) {
      return projection_error(
          "cannot describe retained native collision geometry " +
          geometry_object.name + ": " + error.what());
    }
  }
  for (const auto& body : projection.component_bodies) {
    if (body.geometries.empty()) {
      return projection_error(
          "compiler-owned component collision body has no retained native "
          "geometry: " +
          body.body_id);
    }
  }

  pinocchio::Data neutral_data(shared_model->model);
  const Eigen::VectorXd neutral = pinocchio::neutral(shared_model->model);
  pinocchio::framesForwardKinematics(shared_model->model, neutral_data,
                                    neutral);
  const auto cell_frame_id =
      shared_model->model.getFrameId(compiled_cell_frame_name);
  if (cell_frame_id >= neutral_data.oMf.size()) {
    return projection_error(
        "retained component collision projection cell frame index is out of "
        "range");
  }
  const pinocchio::SE3 world_from_model =
      neutral_data.oMf[cell_frame_id].inverse();

  using FrameKey =
      std::tuple<core::ControllableComponentId, core::ModelInstanceId,
                 core::FrameRoleId>;
  std::set<FrameKey> frame_keys;
  std::set<OccurrenceKey> framed_occurrences;
  projection.component_model_frames.reserve(frames.size());
  for (const auto& input : frames) {
    const OccurrenceKey occurrence{
        input.controllable_component_id, input.model_instance_id,
        input.model_revision};
    if (input.controllable_component_id.empty() ||
        input.model_instance_id.empty() || input.model_revision.empty() ||
        input.frame_role_id.empty() || input.compiled_frame_name.empty() ||
        !collision_occurrences.contains(occurrence) ||
        !shared_model->model.existFrame(input.compiled_frame_name)) {
      return projection_error(
          "retained component-model frame mapping is incomplete, foreign, "
          "or names an unknown compiled frame");
    }
    const FrameKey frame_key{input.controllable_component_id,
                             input.model_instance_id, input.frame_role_id};
    if (!frame_keys.insert(frame_key).second) {
      return projection_error(
          "retained component-model frame mapping is duplicated");
    }
    const auto frame_id =
        shared_model->model.getFrameId(input.compiled_frame_name);
    if (frame_id >= neutral_data.oMf.size()) {
      return projection_error(
          "retained component-model frame index is out of range: " +
          input.compiled_frame_name);
    }
    const pinocchio::SE3 world_from_frame =
        world_from_model * neutral_data.oMf[frame_id];
    projection.component_model_frames.push_back({
        input.controllable_component_id,
        input.model_instance_id,
        input.frame_role_id,
        input.compiled_frame_name,
        input.model_revision,
        pose_from_se3(world_from_frame),
    });
    framed_occurrences.insert(std::move(occurrence));
  }
  for (const auto& occurrence : collision_occurrences) {
    if (!framed_occurrences.contains(occurrence)) {
      return projection_error(
          "component/model collision occurrence has no retained frame "
          "projection");
    }
  }

  std::sort(
      projection.component_model_frames.begin(),
      projection.component_model_frames.end(),
      [](const core::CollisionComponentModelFrame& left,
         const core::CollisionComponentModelFrame& right) {
        return std::tie(left.controllable_component_id,
                        left.model_instance_id, left.frame_role_id) <
               std::tie(right.controllable_component_id,
                        right.model_instance_id, right.frame_role_id);
      });
  std::sort(projection.component_bodies.begin(),
            projection.component_bodies.end(),
            [](const core::CollisionWorldBody& left,
               const core::CollisionWorldBody& right) {
              return left.body_id < right.body_id;
            });
  return core::Result<RetainedComponentCollisionProjection>::Ok(
      std::move(projection));
}

core::Result<RetainedCarriedObjectParentProjection>
ManyForgeCollisionEngine::resolve_retained_carried_object_parent(
    SharedModelPtr shared_model,
    const std::string& compiled_carrier_frame_name,
    std::span<const RetainedComponentCollisionBodyProjectionInput>
        target_bodies) {
  if (!shared_model) {
    return carried_parent_projection_error(
        "carried-object parent resolution requires a loaded model");
  }
  if (compiled_carrier_frame_name.empty() ||
      !shared_model->model.existFrame(compiled_carrier_frame_name)) {
    return carried_parent_projection_error(
        "carried-object carrier frame is absent from the loaded model");
  }
  if (target_bodies.empty()) {
    return carried_parent_projection_error(
        "carried-object command target has no collision-body candidates");
  }

  const auto carrier_frame_id =
      shared_model->model.getFrameId(compiled_carrier_frame_name);
  if (carrier_frame_id >= shared_model->model.frames.size()) {
    return carried_parent_projection_error(
        "carried-object carrier frame index is out of range");
  }
  const auto& carrier_frame = shared_model->model.frames[carrier_frame_id];

  std::map<std::string,
           const RetainedComponentCollisionBodyProjectionInput*, std::less<>>
      rigid_candidates_by_frame;
  bool has_exact_carrier_body = false;
  for (const auto& candidate : target_bodies) {
    if (candidate.controllable_component_id.empty() ||
        candidate.model_instance_id.empty() ||
        candidate.model_revision.empty() ||
        candidate.collision_body_role_id.empty() ||
        candidate.compiled_frame_name.empty() ||
        !shared_model->model.existFrame(candidate.compiled_frame_name)) {
      return carried_parent_projection_error(
          "carried-object target collision-body mapping is incomplete or "
          "names an unknown compiled frame");
    }
    const auto candidate_frame_id =
        shared_model->model.getFrameId(candidate.compiled_frame_name);
    if (candidate_frame_id >= shared_model->model.frames.size()) {
      return carried_parent_projection_error(
          "carried-object target collision-body frame index is out of range");
    }
    const auto& candidate_frame =
        shared_model->model.frames[candidate_frame_id];
    if (candidate_frame.parentJoint != carrier_frame.parentJoint) {
      continue;
    }
    const bool exact_carrier_body =
        candidate.compiled_frame_name == compiled_carrier_frame_name;
    if (has_exact_carrier_body && !exact_carrier_body) {
      continue;
    }
    if (exact_carrier_body && !has_exact_carrier_body) {
      rigid_candidates_by_frame.clear();
      has_exact_carrier_body = true;
    }
    const auto [existing, inserted] = rigid_candidates_by_frame.emplace(
        candidate.compiled_frame_name, &candidate);
    if (!inserted &&
        component_collision_body_id(candidate) <
            component_collision_body_id(*existing->second)) {
      existing->second = &candidate;
    }
  }

  if (rigid_candidates_by_frame.empty()) {
    return carried_parent_projection_error(
        "carried-object carrier frame has no rigid collision-body parent in "
        "the addressed command target");
  }
  if (rigid_candidates_by_frame.size() != 1U) {
    return carried_parent_projection_error(
        "carried-object carrier frame has multiple rigid collision-body "
        "parents in the addressed command target");
  }
  const auto* selected = rigid_candidates_by_frame.begin()->second;

  const auto selected_frame_id =
      shared_model->model.getFrameId(selected->compiled_frame_name);
  const auto& selected_frame = shared_model->model.frames[selected_frame_id];
  const pinocchio::SE3 parent_frame_from_carrier =
      selected_frame.placement.inverse() * carrier_frame.placement;
  return core::Result<RetainedCarriedObjectParentProjection>::Ok({
      component_collision_body_id(*selected),
      selected->compiled_frame_name,
      pose_from_se3(parent_frame_from_carrier),
  });
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
    std::string configuration_error;
    auto configuration =
        full_configuration_from_query(query, configuration_error);
    if (!configuration.has_value()) {
      finish_with_duration();
      result.engine_status =
          core::CollisionEngineStatus::kDiagnosticIncomplete;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["robot_state_error"] =
          std::move(configuration_error);
      return result;
    }
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
    result = check_locked(query, &snapshot, &cancel, *configuration);
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
    const core::CollisionCancellationToken* cancel,
    const Eigen::VectorXd& configuration) {
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
    const Eigen::VectorXd& q = configuration;

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
  prepared->collision_model = shared_model_->base_collision_model;

  std::map<core::CollisionBodyId, const core::CollisionWorldBody*> body_by_id;
  std::map<std::string, core::CollisionBodyId> component_id_by_frame;
  std::map<core::CollisionBodyId, bool> motion_relevant_by_body;
  bool has_world_fixed_body = false;
  for (const auto& body : snapshot.bodies) {
    if (!body_by_id.emplace(body.body_id, &body).second) {
      return "duplicate collision body id: " + body.body_id;
    }
    if (body.owner_kind != core::CollisionOwnerKind::kComponentModelBody) {
      continue;
    }
    const auto collision_index =
        component_body_collision_index(body.body_id);
    if (!collision_index) {
      return "invalid component-model collision body id: " + body.body_id;
    }
    if (body.frame_id.empty() ||
        !shared_model_->model.existFrame(body.frame_id)) {
      return "component-model body frame not present in engine model: " +
             body.frame_id + " for body " + body.body_id;
    }
    if (!component_id_by_frame.emplace(body.frame_id, body.body_id).second) {
      return "multiple component-model collision bodies resolve to compiled "
             "frame: " +
             body.frame_id;
    }
    motion_relevant_by_body[body.body_id] = !link_is_static_wrt_group(
        shared_model_->model, body.frame_id,
        shared_model_->group_joint_names);
  }

  for (const auto& body : snapshot.bodies) {
    if (body.owner_kind == core::CollisionOwnerKind::kComponentModelBody) {
      if (body.parent_body_id.has_value()) {
        return "component-model body cannot declare parent_body_id: " +
               body.body_id;
      }
      continue;
    }
    if (body.parent_body_id.has_value()) {
      if (body.owner_kind != core::CollisionOwnerKind::kSceneResource) {
        return "only scene-resource bodies may declare parent_body_id: " +
               body.body_id;
      }
      const auto parent = body_by_id.find(*body.parent_body_id);
      if (parent == body_by_id.end() ||
          parent->second->owner_kind !=
              core::CollisionOwnerKind::kComponentModelBody) {
        return "scene-resource parent_body_id is not a component-model body: " +
               body.body_id;
      }
      motion_relevant_by_body[body.body_id] =
          motion_relevant_by_body[parent->first];
    } else {
      motion_relevant_by_body[body.body_id] = false;
      has_world_fixed_body = true;
    }
  }

  std::map<std::string, core::CollisionBodyId> builtin_raw_to_body_id;
  std::map<core::CollisionBodyId, std::size_t> native_geometry_count_by_body;
  for (const auto& geometry_object :
       shared_model_->base_collision_model.geometryObjects) {
    if (geometry_object.parentFrame >= shared_model_->model.frames.size()) {
      return "retained native geometry has an invalid parent frame: " +
             geometry_object.name;
    }
    const auto frame_name =
        shared_model_->model.frames[geometry_object.parentFrame].name;
    const auto frame_it = component_id_by_frame.find(frame_name);
    if (frame_it == component_id_by_frame.end()) {
      return "retained native geometry has no component-model body: " +
             geometry_object.name + " (frame=" + frame_name + ")";
    }
    if (!builtin_raw_to_body_id
             .emplace(geometry_object.name, frame_it->second)
             .second) {
      return "retained native geometry name is not unique: " +
             geometry_object.name;
    }
    ++native_geometry_count_by_body[frame_it->second];
  }
  for (const auto& [frame_name, body_id] : component_id_by_frame) {
    const auto body = body_by_id.find(body_id);
    const auto native_count = native_geometry_count_by_body.find(body_id);
    if (body == body_by_id.end() ||
        native_count == native_geometry_count_by_body.end()) {
      return "component-model body has no retained native geometry mapping: " +
             body_id + " (frame=" + frame_name + ")";
    }
    if (body->second->geometries.size() != native_count->second) {
      return "component-model body geometry count differs from the retained "
             "native model: " +
             body_id;
    }
  }

  std::set<std::pair<std::string, std::string>> active_component_body_pairs;
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
    active_component_body_pairs.insert(canonical_pair(a->second, b->second));
  }

  pinocchio::SE3 model_from_world = pinocchio::SE3::Identity();
  std::optional<pinocchio::SE3> world_from_model;
  if (!snapshot.component_model_frames.empty()) {
    pinocchio::Data neutral_data(shared_model_->model);
    const Eigen::VectorXd q_neutral = pinocchio::neutral(shared_model_->model);
    pinocchio::framesForwardKinematics(shared_model_->model, neutral_data, q_neutral);
    for (const auto& frame : snapshot.component_model_frames) {
      if (!shared_model_->model.existFrame(frame.compiled_frame_name)) {
        return "component_model_frame compiled frame not present in engine model: " +
               frame.compiled_frame_name;
      }
      const auto frame_id =
          shared_model_->model.getFrameId(frame.compiled_frame_name);
      if (frame_id >= neutral_data.oMf.size()) {
        return "component_model_frame index out of range: " +
               frame.compiled_frame_name;
      }
      const pinocchio::SE3 model_from_frame = neutral_data.oMf[frame_id];
      const pinocchio::SE3 candidate_world_from_model =
          pose_to_se3(frame.world_pose) * model_from_frame.inverse();
      if (world_from_model.has_value() &&
          !world_from_model->toHomogeneousMatrix().isApprox(
              candidate_world_from_model.toHomogeneousMatrix(), 1e-9)) {
        return "component_model_frames imply inconsistent collision-world "
               "transforms";
      }
      world_from_model = candidate_world_from_model;
    }
    model_from_world = world_from_model->inverse();
  } else if (has_world_fixed_body) {
    return "world-fixed scene/runtime bodies require component_model_frames";
  }

  std::size_t index = 0;
  for (const auto& body : snapshot.bodies) {
    if (body.geometries.empty()) {
      return "manyforge_collision requires at least one geometry per body: " +
             body.body_id;
    }
    if (body.owner_kind == core::CollisionOwnerKind::kComponentModelBody) {
      // Component-model geometry is already present in the exact retained
      // Pinocchio geometry model. The snapshot body supplies stable logical
      // identity and revision binding; adding its descriptors again would
      // duplicate physical geometry and distort collision results.
      continue;
    }
    std::string target_frame;
    if (body.parent_body_id.has_value()) {
      const auto parent = body_by_id.find(*body.parent_body_id);
      if (parent == body_by_id.end()) {
        return "carried scene-resource parent is absent: " + body.body_id;
      }
      target_frame = parent->second->frame_id;
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
            compose_pose_to_se3(body.pose, geom.local_pose);
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

  // Revision-stable logical mapping for both retained native model geometry
  // and snapshot-added scene/runtime geometry. Public body IDs never encode
  // generated link or backend geometry names.
  prepared->raw_to_body_map = std::move(builtin_raw_to_body_id);
  for (const auto& [raw_name, body_id] : prepared->raw_name_to_body_id) {
    if (!prepared->raw_to_body_map.emplace(raw_name, body_id).second) {
      return "snapshot-added geometry aliases retained native geometry: " +
             raw_name;
    }
  }

  std::map<std::string, std::size_t> mapped_index_by_name;
  for (std::size_t i = 0; i < prepared->collision_model.geometryObjects.size();
       ++i) {
    const auto& name = prepared->collision_model.geometryObjects[i].name;
    if (prepared->raw_to_body_map.count(name) > 0) {
      mapped_index_by_name[name] = i;
    }
  }
  std::set<std::pair<std::size_t, std::size_t>> active_pair_indices;
  for (const auto& pair : prepared->collision_model.collisionPairs) {
    const auto first = std::min(pair.first, pair.second);
    const auto second = std::max(pair.first, pair.second);
    active_pair_indices.insert({first, second});
  }
  for (auto it_a = mapped_index_by_name.begin();
       it_a != mapped_index_by_name.end(); ++it_a) {
    auto body_a = prepared->raw_to_body_map.find(it_a->first);
    if (body_a == prepared->raw_to_body_map.end()) {
      continue;
    }
    auto it_b = it_a;
    ++it_b;
    for (; it_b != mapped_index_by_name.end(); ++it_b) {
      auto body_b = prepared->raw_to_body_map.find(it_b->first);
      if (body_b == prepared->raw_to_body_map.end()) {
        continue;
      }
      if (body_a->second == body_b->second) {
        // Geometry elements belonging to one logical collision body are a
        // compound shape, not a policy-visible self-collision pair.
        continue;
      }
      const bool a_motion_relevant = motion_relevant_by_body[body_a->second];
      const bool b_motion_relevant = motion_relevant_by_body[body_b->second];
      if (!a_motion_relevant && !b_motion_relevant) {
        continue;
      }

      const auto logical_a = body_by_id.find(body_a->second);
      const auto logical_b = body_by_id.find(body_b->second);
      if (logical_a == body_by_id.end() || logical_b == body_by_id.end()) {
        return "native collision geometry maps to an unknown logical body";
      }
      const bool a_component_body =
          logical_a->second->owner_kind ==
          core::CollisionOwnerKind::kComponentModelBody;
      const bool b_component_body =
          logical_b->second->owner_kind ==
          core::CollisionOwnerKind::kComponentModelBody;
      if (a_component_body && b_component_body &&
          active_component_body_pairs.count(
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
