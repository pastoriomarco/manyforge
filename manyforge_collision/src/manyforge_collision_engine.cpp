#include <manyforge_collision/manyforge_collision_engine.hpp>

#include <Eigen/Dense>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <sstream>
#include <utility>

#include <pinocchio/algorithm/geometry.hpp>
#include <pinocchio/algorithm/joint-configuration.hpp>
#include <pinocchio/collision/collision.hpp>
#include <pinocchio/parsers/srdf.hpp>
#include <pinocchio/parsers/urdf.hpp>

#include <hpp/fcl/shape/geometric_shapes.h>

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
  return snapshot_mode && capability == "body_pair_only";
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

std::pair<std::string, std::string> canonical_pair(std::string a,
                                                   std::string b) {
  if (b < a) {
    std::swap(a, b);
  }
  return {std::move(a), std::move(b)};
}

std::vector<std::pair<std::string, std::string>>
collision_pairs_from_geometry_data(
    const pinocchio::GeometryModel& collision_model,
    const pinocchio::GeometryData& collision_data) {
  std::vector<std::pair<std::string, std::string>> pairs;
  const auto pair_count = std::min(collision_model.collisionPairs.size(),
                                   collision_data.collisionResults.size());
  for (std::size_t k = 0; k < pair_count; ++k) {
    const auto& result = collision_data.collisionResults[k];
    if (!result.isCollision()) {
      continue;
    }
    const auto& pair = collision_model.collisionPairs[k];
    if (pair.first >= collision_model.geometryObjects.size() ||
        pair.second >= collision_model.geometryObjects.size()) {
      continue;
    }
    pairs.push_back(canonical_pair(
        collision_model.geometryObjects[pair.first].name,
        collision_model.geometryObjects[pair.second].name));
  }
  std::sort(pairs.begin(), pairs.end());
  pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
  return pairs;
}

}  // namespace

ManyForgeCollisionEngine::ManyForgeCollisionEngine(
    const std::string& model_name,
    const std::filesystem::path& urdf_path,
    const std::filesystem::path& srdf_path,
    const std::vector<std::filesystem::path>& package_paths,
    std::string group_name)
    : group_name_(std::move(group_name)),
      engine_id_("manyforge_collision/" + model_name),
      engine_version_(MANYFORGE_COLLISION_ENGINE_VERSION) {
  pinocchio::urdf::buildModel(urdf_path.string(), model_, false, false);
  std::vector<std::string> package_path_strings;
  package_path_strings.reserve(package_paths.size());
  for (const auto& path : package_paths) {
    package_path_strings.push_back(path.string());
  }
  pinocchio::urdf::buildGeom(model_,
                             urdf_path.string(),
                             pinocchio::COLLISION,
                             base_collision_model_,
                             package_path_strings);
  if (!srdf_path.empty() && std::filesystem::exists(srdf_path)) {
    pinocchio::srdf::removeCollisionPairs(model_,
                                          base_collision_model_,
                                          srdf_path.string(),
                                          false);
  }
  group_joint_names_ =
      group_joint_names_from_srdf(srdf_path, urdf_path, group_name_);

  capability_.engine_id = engine_id_;
  capability_.engine_version = engine_version_;
  capability_.library_lineage = "manyforge_collision_pinocchio_coal";
  capability_.supported_geometry_kinds = {"box", "sphere", "cylinder"};
  capability_.supports_distance_query = false;
  capability_.supports_contact_points = false;
  capability_.supports_penetration_depth = false;
  capability_.supports_continuous_collision = false;
  capability_.thread_safety =
      core::CollisionEngineThreadSafetyClass::kSingleContextExclusive;
  capability_.deterministic_ordering = true;
  capability_.known_unsupported_cases = {
      "scene_object_mesh_without_approved_proxy",
      "scene_object_compound_geometry",
      "distance_query",
      "continuous_collision"};
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
  auto finish_with_duration = [&]() {
    const auto t_end = std::chrono::steady_clock::now();
    result.query_duration_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start)
            .count();
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
    result.engine_status = core::CollisionEngineStatus::kTimeout;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    return result;
  }

  try {
    std::lock_guard<std::mutex> scene_lock(scene_mutex_);
    if (auto sync_error = sync_snapshot_locked(snapshot);
        sync_error.has_value()) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kUnsupportedGeometry;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["snapshot_sync_error"] = *sync_error;
      return result;
    }
    if (cancel.is_cancel_requested()) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kCancelled;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      result.raw_diagnostics["cancel_reason"] = cancel.reason();
      return result;
    }
    result = check_locked(query, &snapshot, &cancel);
    // The snapshot overload may need to synchronize a new immutable world into
    // the engine-local collision model before the query can run. The timeout
    // budget and telemetry must cover that full blocking call, not only the
    // inner collision check.
    finish_with_duration();
    if (result.engine_status == core::CollisionEngineStatus::kOk &&
        query.timeout_budget_ns &&
        result.query_duration_ns &&
        *result.query_duration_ns > *query.timeout_budget_ns) {
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

  const auto t_start = std::chrono::steady_clock::now();
  auto finish_with_duration = [&]() {
    const auto t_end = std::chrono::steady_clock::now();
    result.query_duration_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start)
            .count();
  };

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
    result.engine_status = core::CollisionEngineStatus::kTimeout;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    return result;
  }

  try {
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
      if (!active_collision_model_ || !active_collision_data_) {
        finish_with_duration();
        result.engine_status =
            core::CollisionEngineStatus::kDiagnosticIncomplete;
        result.decision = core::CollisionDecisionResult::kInconclusiveFail;
        result.raw_diagnostics["snapshot_sync_error"] =
            "snapshot collision model is not initialized";
        return result;
      }
      pinocchio::Data model_data(model_);
      pinocchio::updateGeometryPlacements(model_,
                                          model_data,
                                          *active_collision_model_,
                                          *active_collision_data_,
                                          q);
      result.in_collision =
          pinocchio::computeCollisions(model_,
                                       model_data,
                                       *active_collision_model_,
                                       *active_collision_data_,
                                       q,
                                       /* stop_at_first_collision */ true);
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
    if (result.in_collision) {
      std::vector<std::pair<std::string, std::string>> raw_pairs;
      if (snapshot != nullptr) {
        pinocchio::Data model_data(model_);
        pinocchio::updateGeometryPlacements(model_,
                                            model_data,
                                            *active_collision_model_,
                                            *active_collision_data_,
                                            q);
        pinocchio::computeCollisions(model_,
                                     model_data,
                                     *active_collision_model_,
                                     *active_collision_data_,
                                     q,
                                     /* stop_at_first_collision */ false);
        raw_pairs = collision_pairs_from_geometry_data(*active_collision_model_,
                                                       *active_collision_data_);
      } else {
        raw_pairs = {};
      }
      if (cancel != nullptr && cancel->is_cancel_requested()) {
        finish_with_duration();
        result.engine_status = core::CollisionEngineStatus::kCancelled;
        result.decision = core::CollisionDecisionResult::kInconclusiveFail;
        result.raw_diagnostics["cancel_reason"] = cancel->reason();
        return result;
      }
      std::map<std::string, core::CollisionBodyId> raw_to_body =
          synced_raw_name_to_body_id_;

      if (snapshot != nullptr) {
        const auto& collision_model = *active_collision_model_;
        std::set<std::string> scene_raw_names;
        for (const auto& name : synced_scene_geometry_names_) {
          scene_raw_names.insert(name);
        }
        std::map<std::string, std::map<int, core::CollisionBodyId>>
            robot_ids_by_link_index;
        for (const auto& body : snapshot->bodies) {
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
          if (geometry_object.parentFrame >= model_.frames.size()) {
            continue;
          }
          const auto link_name = model_.frames[geometry_object.parentFrame].name;
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
      }

      bool any_unknown = raw_pairs.empty();
      if (raw_pairs.empty()) {
        result.raw_diagnostics["collision_diagnostics_available"] = "false";
      }
      for (const auto& [raw_a, raw_b] : raw_pairs) {
        auto a = raw_to_body.find(raw_a);
        auto b = raw_to_body.find(raw_b);
        if (a == raw_to_body.end() || b == raw_to_body.end()) {
          any_unknown = true;
          result.raw_diagnostics["unknown_collision_pairs"] +=
              raw_a + " <-> " + raw_b + ";";
          continue;
        }
        core::CollisionPairId pair{a->second, b->second};
        if (pair.body_b < pair.body_a) {
          std::swap(pair.body_a, pair.body_b);
        }
        result.normalized_pairs.push_back(std::move(pair));
      }
      std::sort(result.normalized_pairs.begin(), result.normalized_pairs.end());
      result.normalized_pairs.erase(
          std::unique(result.normalized_pairs.begin(),
                      result.normalized_pairs.end()),
          result.normalized_pairs.end());
      if (!result.normalized_pairs.empty()) {
        result.contact_body_a = result.normalized_pairs.front().body_a;
        result.contact_body_b = result.normalized_pairs.front().body_b;
      }
      if (any_unknown) {
        finish_with_duration();
        result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
        result.decision = core::CollisionDecisionResult::kInconclusiveFail;
        return result;
      }
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
  if (static_cast<Eigen::Index>(positions.size()) == model_.nq) {
    Eigen::VectorXd q(model_.nq);
    for (std::size_t i = 0; i < positions.size(); ++i) {
      q(static_cast<Eigen::Index>(i)) = positions[i];
    }
    return q;
  }

  Eigen::VectorXd q = pinocchio::neutral(model_);
  auto fill_joint = [&](const std::string& joint_name,
                        double value) -> bool {
    if (!model_.existJointName(joint_name)) {
      error = "joint not present in model: " + joint_name;
      return false;
    }
    const auto joint_id = model_.getJointId(joint_name);
    if (joint_id >= model_.idx_qs.size() ||
        joint_id >= model_.nqs.size()) {
      error = "joint index out of range for: " + joint_name;
      return false;
    }
    if (model_.nqs[joint_id] != 1) {
      error = "joint is not scalar in current primitive profile: " + joint_name;
      return false;
    }
    q(model_.idx_qs[joint_id]) = value;
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

  if (group_joint_names_.size() == positions.size()) {
    for (std::size_t i = 0; i < positions.size(); ++i) {
      if (!fill_joint(group_joint_names_[i], positions[i])) {
        return std::nullopt;
      }
    }
    return q;
  }

  error = "robot_state.positions_rad size " + std::to_string(positions.size()) +
          " does not match model.nq " + std::to_string(model_.nq) +
          ", robot_state.joint_names, or SRDF group '" + group_name_ + "'";
  return std::nullopt;
}

std::optional<std::string> ManyForgeCollisionEngine::sync_snapshot_locked(
    const core::CollisionWorldSnapshot& snapshot) {
  if (synced_snapshot_revision_ &&
      synced_snapshot_revision_->revision_id == snapshot.revision.revision_id &&
      synced_snapshot_revision_->content_hash == snapshot.revision.content_hash) {
    return std::nullopt;
  }

  synced_scene_geometry_names_.clear();
  synced_raw_name_to_body_id_.clear();
  synced_snapshot_revision_.reset();
  active_collision_model_.reset();
  active_collision_data_.reset();

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
  for (const auto& geometry_object : base_collision_model_.geometryObjects) {
    if (geometry_object.parentFrame >= model_.frames.size()) {
      continue;
    }
    const auto link_name = model_.frames[geometry_object.parentFrame].name;
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
  for (const auto& pair : base_collision_model_.collisionPairs) {
    if (pair.first >= base_collision_model_.geometryObjects.size() ||
        pair.second >= base_collision_model_.geometryObjects.size()) {
      continue;
    }
    auto a = builtin_raw_to_body_id.find(
        base_collision_model_.geometryObjects[pair.first].name);
    auto b = builtin_raw_to_body_id.find(
        base_collision_model_.geometryObjects[pair.second].name);
    if (a == builtin_raw_to_body_id.end() ||
        b == builtin_raw_to_body_id.end()) {
      continue;
    }
    active_robot_body_pairs.insert(canonical_pair(a->second, b->second));
  }

  active_collision_model_.emplace();
  std::size_t index = 0;
  for (const auto& body : snapshot.bodies) {
    if (body.geometries.size() != 1) {
      return "manyforge_collision primitive scope requires exactly one geometry per body: " +
             body.body_id;
    }
    const auto& geom = body.geometries.front();
    const std::string raw_name = sanitized_geometry_name(body.body_id, index++);
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

    std::shared_ptr<hpp::fcl::CollisionGeometry> shape;
    switch (geom.geometry_kind) {
      case core::CollisionGeometryKind::kBox: {
        if (!geom.box_dimensions_m) {
          return "box geometry missing dimensions: " + body.body_id;
        }
        shape = std::make_shared<hpp::fcl::Box>(geom.box_dimensions_m->x_m,
                                                geom.box_dimensions_m->y_m,
                                                geom.box_dimensions_m->z_m);
        break;
      }
      case core::CollisionGeometryKind::kSphere: {
        if (!geom.sphere_radius_m) {
          return "sphere geometry missing radius: " + body.body_id;
        }
        shape = std::make_shared<hpp::fcl::Sphere>(*geom.sphere_radius_m);
        break;
      }
      case core::CollisionGeometryKind::kCylinder: {
        if (!geom.cylinder_radius_m || !geom.cylinder_height_m) {
          return "cylinder geometry missing dimensions: " + body.body_id;
        }
        shape = std::make_shared<hpp::fcl::Cylinder>(*geom.cylinder_radius_m,
                                                     *geom.cylinder_height_m);
        break;
      }
      case core::CollisionGeometryKind::kCapsule:
      case core::CollisionGeometryKind::kConvexMesh:
      case core::CollisionGeometryKind::kTriangleMesh:
      case core::CollisionGeometryKind::kCompound:
        return "unsupported scene geometry kind for direct primitive engine: " +
               body.body_id;
    }

    std::optional<pinocchio::GeometryObject> object;
    if (target_frame != "universe") {
      if (!model_.existFrame(target_frame)) {
        return "target frame not present in model: " + target_frame +
               " for body " + body.body_id;
      }
      const auto frame_id = model_.getFrameId(target_frame);
      const auto& frame = model_.frames[frame_id];
      const auto placement =
          body.owner_kind == core::CollisionOwnerKind::kRobotLink
              ? pose_to_se3(geom.local_pose)
              : compose_pose_to_se3(body.pose, geom.local_pose);
      object.emplace(raw_name,
                     frame.parentJoint,
                     frame_id,
                     placement,
                     shape);
    } else {
      object.emplace(raw_name,
                     pinocchio::JointIndex(0),
                     pinocchio::FrameIndex(0),
                     compose_pose_to_se3(body.pose, geom.local_pose),
                     shape);
    }
    active_collision_model_->addGeometryObject(*object, model_);
    synced_scene_geometry_names_.push_back(raw_name);
    synced_raw_name_to_body_id_[raw_name] = body.body_id;
  }

  std::map<std::string, std::size_t> synced_index_by_name;
  for (std::size_t i = 0; i < active_collision_model_->geometryObjects.size();
       ++i) {
    const auto& name = active_collision_model_->geometryObjects[i].name;
    if (synced_raw_name_to_body_id_.count(name) > 0) {
      synced_index_by_name[name] = i;
    }
  }
  std::set<std::pair<std::size_t, std::size_t>> active_pair_indices;
  for (const auto& pair : active_collision_model_->collisionPairs) {
    const auto first = std::min(pair.first, pair.second);
    const auto second = std::max(pair.first, pair.second);
    active_pair_indices.insert({first, second});
  }
  for (auto it_a = synced_index_by_name.begin();
       it_a != synced_index_by_name.end(); ++it_a) {
    auto body_a = synced_raw_name_to_body_id_.find(it_a->first);
    if (body_a == synced_raw_name_to_body_id_.end()) {
      continue;
    }
    auto it_b = it_a;
    ++it_b;
    for (; it_b != synced_index_by_name.end(); ++it_b) {
      auto body_b = synced_raw_name_to_body_id_.find(it_b->first);
      if (body_b == synced_raw_name_to_body_id_.end()) {
        continue;
      }
      const bool a_robot = body_a->second.rfind("robot/", 0) == 0;
      const bool b_robot = body_b->second.rfind("robot/", 0) == 0;
      if (a_robot && b_robot &&
          active_robot_body_pairs.count(
              canonical_pair(body_a->second, body_b->second)) == 0) {
        continue;
      }
      const auto first = std::min(it_a->second, it_b->second);
      const auto second = std::max(it_a->second, it_b->second);
      if (active_pair_indices.insert({first, second}).second) {
        active_collision_model_->addCollisionPair(
            pinocchio::CollisionPair(first, second));
      }
    }
  }
  active_collision_data_.emplace(*active_collision_model_);

  synced_snapshot_revision_ = snapshot.revision;
  return std::nullopt;
}

}  // namespace manyforge::collision
