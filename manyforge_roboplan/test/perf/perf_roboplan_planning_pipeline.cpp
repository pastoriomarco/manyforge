#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>
#include <manyforge_roboplan/roboplan_planner_options.hpp>

#include <manyforge_core/geometry/pose_math.hpp>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <time.h>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::uint64_t> g_allocation_count{0};
std::atomic<std::uint64_t> g_allocation_bytes{0};

void record_allocation(std::size_t size) noexcept {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
    g_allocation_bytes.fetch_add(
        static_cast<std::uint64_t>(size), std::memory_order_relaxed);
  }
}

void* allocate_bytes(std::size_t size) {
  return std::malloc(size == 0 ? 1 : size);
}

void* allocate_aligned_bytes(std::size_t size, std::size_t alignment) {
  void* pointer = nullptr;
  if (posix_memalign(&pointer, alignment, size == 0 ? 1 : size) != 0) {
    return nullptr;
  }
  return pointer;
}

void free_bytes(void* pointer) { std::free(pointer); }

}  // namespace

void* operator new(std::size_t size) {
  record_allocation(size);
  if (void* pointer = allocate_bytes(size)) {
    return pointer;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  record_allocation(size);
  if (void* pointer =
          allocate_aligned_bytes(size, static_cast<std::size_t>(alignment))) {
    return pointer;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return operator new(size, alignment);
}
void operator delete(void* pointer) noexcept { free_bytes(pointer); }
void operator delete[](void* pointer) noexcept { free_bytes(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { free_bytes(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept {
  free_bytes(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
  free_bytes(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
  free_bytes(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  free_bytes(pointer);
}
void operator delete[](void* pointer, std::size_t,
                       std::align_val_t) noexcept {
  free_bytes(pointer);
}

namespace {

using boost::property_tree::ptree;
using manyforge::core::AddObjectRequestDTO;
using manyforge::core::AttachObjectRequestDTO;
using manyforge::core::BoxShapeDTO;
using manyforge::core::CollisionContactPolicyMode;
using manyforge::core::ContactReleaseEvidenceMode;
using manyforge::core::CollisionPairId;
using manyforge::core::CollisionPolicySnapshot;
using manyforge::core::ContactGraspPolicyEntry;
using manyforge::core::JointGoalDTO;
using manyforge::core::MeshShapeDTO;
using manyforge::core::PlanRequestDTO;
using manyforge::core::PlanResultDTO;
using manyforge::core::Pose3DTO;
using manyforge::core::PoseGoalDTO;
using manyforge::core::ReferenceDTO;
using manyforge::core::SceneRevisionBindingDTO;
using manyforge::core::ExecutionStateUpdateMode;
using manyforge::core::FkRequestDTO;
using manyforge::core::SetRobotBasePoseRequestDTO;
using manyforge::core::SetCollisionPolicyRequestDTO;
using manyforge::core::SupportContactDTO;
using manyforge::core::SupportContactPolicyEntry;
using manyforge::core::Vector3DTO;
using manyforge::roboplan_backend::IRoboPlanPlanningPerformanceObserver;
using manyforge::roboplan_backend::NativeKernelBundle;
using manyforge::roboplan_backend::RoboPlanAllocationProbeSnapshot;
using manyforge::roboplan_backend::RoboPlanCollisionProbeSnapshot;
using manyforge::roboplan_backend::RoboPlanPlannerBackend;
using manyforge::roboplan_backend::RoboPlanPlanningPipelineMetrics;
using manyforge::roboplan_backend::create_native_kernel;
using manyforge::roboplan_backend::production_planning_ik_options;
using manyforge::roboplan_backend::production_rrt_options;
using manyforge::roboplan_backend::production_single_shot_ik_options;

struct Options {
  std::filesystem::path repo_root = "/workspace";
  std::filesystem::path corpus_path;
  std::string fixture_id;
  int warmup_repetitions = 5;
  int measured_repetitions = 50;
  std::uint32_t rng_seed = 12345;
  std::optional<std::string> urdf_override;
  std::optional<std::string> srdf_override;
  std::vector<std::string> package_path_overrides;
  enum class PlannerPolicy {
    kCompatibility,
    kRrtStar,
    kBudgetSpending,
  } planner_policy = PlannerPolicy::kCompatibility;
};

std::string_view planner_policy_name(Options::PlannerPolicy policy) {
  switch (policy) {
    case Options::PlannerPolicy::kCompatibility:
      return "compatibility";
    case Options::PlannerPolicy::kRrtStar:
      return "rrt_star";
    case Options::PlannerPolicy::kBudgetSpending:
      return "budget_spending";
  }
  throw std::invalid_argument("unknown planner policy");
}

Options::PlannerPolicy parse_planner_policy(const std::string& value) {
  if (value == "compatibility") {
    return Options::PlannerPolicy::kCompatibility;
  }
  if (value == "rrt_star") {
    return Options::PlannerPolicy::kRrtStar;
  }
  if (value == "budget_spending") {
    return Options::PlannerPolicy::kBudgetSpending;
  }
  throw std::invalid_argument(
      "--planner-policy must be compatibility, rrt_star, or budget_spending");
}

struct Fixture {
  std::string id;
  std::string model_id;
  std::string world_profile;
  std::vector<double> start;
  std::vector<double> goal;
  std::optional<std::string> capture_file;
  bool expected_success = true;
};

struct Model {
  std::string source;
  std::string model_name;
  std::string group_name;
  std::string base_frame;
  std::string tcp_frame;
  std::string urdf_path;
  std::string srdf_path;
  std::vector<std::string> package_paths;
};

template <typename T>
std::vector<T> array_values(const ptree& tree) {
  std::vector<T> values;
  for (const auto& [key, value] : tree) {
    (void)key;
    values.push_back(value.template get_value<T>());
  }
  return values;
}

std::string json_escape(const std::string& value) {
  std::ostringstream out;
  for (const char c : value) {
    switch (c) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        out << c;
    }
  }
  return out.str();
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string arg(argv[index]);
    auto next = [&]() -> std::string {
      if (++index >= argc) {
        throw std::invalid_argument("missing value after " + arg);
      }
      return argv[index];
    };
    if (arg == "--repo-root") {
      options.repo_root = next();
    } else if (arg == "--corpus") {
      options.corpus_path = next();
    } else if (arg == "--fixture") {
      options.fixture_id = next();
    } else if (arg == "--warmup") {
      options.warmup_repetitions = std::stoi(next());
    } else if (arg == "--repetitions") {
      options.measured_repetitions = std::stoi(next());
    } else if (arg == "--rng-seed") {
      const auto value = std::stoull(next());
      if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("rng seed exceeds uint32");
      }
      options.rng_seed = static_cast<std::uint32_t>(value);
    } else if (arg == "--planner-policy") {
      options.planner_policy = parse_planner_policy(next());
    } else if (arg == "--urdf") {
      options.urdf_override = next();
    } else if (arg == "--srdf") {
      options.srdf_override = next();
    } else if (arg == "--package-path") {
      options.package_path_overrides.push_back(next());
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }
  if (options.corpus_path.empty() || options.fixture_id.empty()) {
    throw std::invalid_argument("--corpus and --fixture are required");
  }
  if (options.warmup_repetitions < 0 || options.measured_repetitions < 1) {
    throw std::invalid_argument("invalid repetition count");
  }
  return options;
}

Fixture load_fixture(const ptree& root, const std::string& fixture_id) {
  for (const auto& [key, candidate] : root.get_child("planning_fixtures")) {
    (void)key;
    if (candidate.get<std::string>("id") != fixture_id) {
      continue;
    }
    Fixture fixture;
    fixture.id = fixture_id;
    fixture.model_id = candidate.get<std::string>("model");
    fixture.world_profile = candidate.get<std::string>("world_profile");
    fixture.expected_success = candidate.get<bool>("expected.success");
    if (const auto start = candidate.get_child_optional("start_state_rad")) {
      fixture.start = array_values<double>(*start);
    }
    if (const auto goal = candidate.get_child_optional("joint_goal_rad")) {
      fixture.goal = array_values<double>(*goal);
    }
    if (const auto capture_file =
            candidate.get_optional<std::string>("capture_file")) {
      fixture.capture_file = *capture_file;
    }
    if ((fixture.start.empty() || fixture.goal.empty()) &&
        !fixture.capture_file) {
      throw std::invalid_argument(
          "planning fixture requires an inline joint goal or capture file");
    }
    return fixture;
  }
  throw std::invalid_argument("unknown planning fixture: " + fixture_id);
}

Model load_model(const ptree& root, const std::string& model_id,
                 const std::filesystem::path& repo_root) {
  const auto& value = root.get_child("models." + model_id);
  Model model;
  model.source = value.get<std::string>("source");
  model.model_name = value.get<std::string>("model_name");
  model.group_name = value.get<std::string>("group_name");
  model.base_frame = value.get<std::string>("base_frame");
  model.tcp_frame = value.get<std::string>("tcp_frame");
  if (model.source == "manyforge_example_models") {
    model.urdf_path =
        (repo_root / value.get<std::string>("urdf_path")).string();
    model.srdf_path =
        (repo_root / value.get<std::string>("srdf_path")).string();
    for (const auto& package : value.get_child("package_paths")) {
      model.package_paths.push_back(
          (repo_root / package.second.get_value<std::string>()).string());
    }
  }
  return model;
}

std::shared_ptr<NativeKernelBundle> create_bundle(const Model& model) {
  if (model.source == "roboplan_example_models") {
    return create_native_kernel(model.model_name, model.group_name);
  }
  return create_native_kernel(model.model_name, model.group_name,
                              model.urdf_path, model.srdf_path,
                              model.package_paths);
}

Pose3DTO identity_pose(double x, double y, double z) {
  Pose3DTO pose;
  pose.position_m = Vector3DTO{x, y, z};
  pose.orientation.w = 1.0;
  return pose;
}

Pose3DTO captured_pose(const ptree& value) {
  const auto position =
      array_values<double>(value.get_child("position_m"));
  const auto orientation =
      array_values<double>(value.get_child("orientation_xyzw"));
  if (position.size() != 3 || orientation.size() != 4) {
    throw std::invalid_argument("captured pose has invalid dimensions");
  }
  Pose3DTO pose;
  pose.position_m = Vector3DTO{position[0], position[1], position[2]};
  pose.orientation.x = orientation[0];
  pose.orientation.y = orientation[1];
  pose.orientation.z = orientation[2];
  pose.orientation.w = orientation[3];
  return pose;
}

double quaternion_angular_distance(const Pose3DTO& first,
                                   const Pose3DTO& second) {
  const auto& a = first.orientation;
  const auto& b = second.orientation;
  const double a_norm =
      std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
  const double b_norm =
      std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w);
  if (!(a_norm > 0.0) || !(b_norm > 0.0)) {
    return std::numeric_limits<double>::infinity();
  }
  const double dot = std::clamp(
      std::abs((a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) /
               (a_norm * b_norm)),
      0.0, 1.0);
  return 2.0 * std::acos(dot);
}

double pose_translation_distance(const Pose3DTO& first,
                                 const Pose3DTO& second) {
  const double dx = first.position_m.x_m - second.position_m.x_m;
  const double dy = first.position_m.y_m - second.position_m.y_m;
  const double dz = first.position_m.z_m - second.position_m.z_m;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

ReferenceDTO captured_reference(const ptree& value) {
  ReferenceDTO reference;
  const auto type = value.get<std::string>("type");
  if (type == "object") {
    reference.type = ReferenceDTO::Type::kObject;
  } else if (type == "frame") {
    reference.type = ReferenceDTO::Type::kFrame;
  } else if (type == "robot_link") {
    reference.type = ReferenceDTO::Type::kRobotLink;
  } else {
    throw std::invalid_argument("unsupported captured reference type: " + type);
  }
  reference.id = value.get<std::string>("id");
  return reference;
}

Pose3DTO resolve_captured_pose_goal(
    NativeKernelBundle& bundle, const ptree& capture,
    const std::string& base_frame,
    const SceneRevisionBindingDTO& scene_binding) {
  const auto& source = capture.get_child("resolved_motion.source");
  auto target_pose = bundle.reference_resolver->resolve_reference_pose(
      captured_reference(source.get_child("target_reference")), base_frame,
      scene_binding);
  if (!target_pose.ok()) {
    throw std::runtime_error("captured target resolution failed: " +
                             target_pose.error().message);
  }
  auto offset_basis = bundle.reference_resolver->resolve_reference_pose(
      captured_reference(source.get_child("offset_reference")), base_frame,
      scene_binding);
  if (!offset_basis.ok()) {
    throw std::runtime_error("captured offset-reference resolution failed: " +
                             offset_basis.error().message);
  }
  const auto resolved = manyforge::core::multiply_poses(
      manyforge::core::pose_with_position_and_orientation(
          target_pose.value().position_m, offset_basis.value().orientation),
      captured_pose(source.get_child("offset_pose")));
  const auto expected =
      captured_pose(capture.get_child("resolved_motion.pose_goal"));
  constexpr double kCaptureCrossCheckTolerance = 1e-8;
  if (pose_translation_distance(resolved, expected) >
          kCaptureCrossCheckTolerance ||
      quaternion_angular_distance(resolved, expected) >
          kCaptureCrossCheckTolerance) {
    throw std::runtime_error(
        "captured pose goal disagrees with production reference resolution");
  }
  return resolved;
}

std::optional<double> optional_json_double(const ptree& parent,
                                           const std::string& path) {
  const auto raw = parent.get_optional<std::string>(path);
  if (!raw || *raw == "null") {
    return std::nullopt;
  }
  return std::stod(*raw);
}

CollisionPairId captured_collision_pair(const ptree& value) {
  return CollisionPairId{value.get<std::string>("body_a"),
                         value.get<std::string>("body_b")};
}

CollisionPolicySnapshot captured_collision_policy(const ptree& value) {
  CollisionPolicySnapshot policy;
  policy.policy_id = value.get<std::string>("policy_id");
  policy.revision.revision_id =
      value.get<std::uint64_t>("revision.revision_id");
  policy.revision.content_hash =
      value.get<std::string>("revision.content_hash");
  policy.require_full_diagnostics =
      value.get<bool>("require_full_diagnostics");
  for (const auto& [key, pair] :
       value.get_child("allowed_static_static_pairs")) {
    (void)key;
    policy.allowed_static_static_pairs.push_back(
        captured_collision_pair(pair));
  }
  for (const auto& [key, pair] :
       value.get_child("allowed_touch_link_pairs")) {
    (void)key;
    policy.allowed_touch_link_pairs.push_back(captured_collision_pair(pair));
  }
  for (const auto& [key, contact] : value.get_child("support_contacts")) {
    (void)key;
    SupportContactPolicyEntry entry;
    entry.pair = captured_collision_pair(contact.get_child("pair"));
    const auto mode = contact.get<std::string>("mode", "contact_release");
    if (mode == "contact_release") {
      entry.mode = CollisionContactPolicyMode::kContactRelease;
    } else if (mode == "bounded_penetration") {
      entry.mode = CollisionContactPolicyMode::kBoundedPenetration;
      entry.max_contact_penetration_m =
          contact.get<double>("max_contact_penetration_m");
      if (!std::isfinite(entry.max_contact_penetration_m) ||
          entry.max_contact_penetration_m < 0.0) {
        throw std::invalid_argument(
            "captured bounded contact policy has invalid penetration bound");
      }
      policy.support_contacts.push_back(std::move(entry));
      continue;
    } else if (mode == "contact_process") {
      entry.mode = CollisionContactPolicyMode::kContactProcess;
      policy.support_contacts.push_back(std::move(entry));
      continue;
    } else {
      throw std::invalid_argument(
          "captured collision policy has unsupported contact mode: " + mode);
    }
    const auto evidence_mode = contact.get<std::string>(
        "release_evidence_mode", "signed_penetration");
    if (evidence_mode == "signed_penetration") {
      entry.release_evidence_mode =
          ContactReleaseEvidenceMode::kSignedPenetration;
    } else if (evidence_mode == "binary_contact") {
      entry.release_evidence_mode =
          ContactReleaseEvidenceMode::kBinaryContact;
    } else {
      throw std::invalid_argument(
          "captured collision policy has unsupported release evidence mode: " +
          evidence_mode);
    }
    entry.enforce_max_start_penetration =
        contact.get<bool>(
            "enforce_max_start_penetration",
            entry.release_evidence_mode ==
                ContactReleaseEvidenceMode::kSignedPenetration);
    entry.max_start_penetration_m =
        contact.get<double>("max_start_penetration_m", 0.0);
    entry.release_tolerance_m =
        contact.get<double>(
            "release_tolerance_m",
            entry.release_evidence_mode ==
                    ContactReleaseEvidenceMode::kSignedPenetration
                ? 1e-5
                : 0.0);
    entry.min_release_progress_m =
        contact.get<double>("min_release_progress_m", 0.0);
    entry.require_separation_by_end =
        contact.get<bool>("require_separation_by_end", true);
    entry.require_strictly_decreasing_penetration = contact.get<bool>(
        "require_strictly_decreasing_penetration",
        entry.release_evidence_mode ==
            ContactReleaseEvidenceMode::kSignedPenetration);
    entry.allow_recontact = contact.get<bool>("allow_recontact", false);
    if (entry.release_evidence_mode ==
        ContactReleaseEvidenceMode::kBinaryContact) {
      if (entry.enforce_max_start_penetration ||
          entry.max_start_penetration_m != 0.0 ||
          entry.release_tolerance_m != 0.0 ||
          entry.min_release_progress_m != 0.0 ||
          entry.require_strictly_decreasing_penetration) {
        throw std::invalid_argument(
            "captured binary contact policy contains metric constraints");
      }
    }
    policy.support_contacts.push_back(std::move(entry));
  }
  for (const auto& [key, grasp] : value.get_child("contact_grasps")) {
    (void)key;
    ContactGraspPolicyEntry entry;
    entry.policy_id = grasp.get<std::string>("policy_id");
    entry.selected_object_id =
        grasp.get<std::string>("selected_object_id");
    entry.allowed_robot_links =
        array_values<std::string>(grasp.get_child("allowed_robot_links"));
    entry.max_penetration_m = grasp.get<double>("max_penetration_m", 0.0);
    entry.terminal_window_start_fraction =
        grasp.get<double>("terminal_window_start_fraction", 0.8);
    policy.contact_grasps.push_back(std::move(entry));
  }
  return policy;
}

PlanRequestDTO captured_plan_request(
    const ptree& capture, const std::vector<std::string>& expected_joint_names,
    const SceneRevisionBindingDTO& replay_binding) {
  const auto& value = capture.get_child("plan_request");
  PlanRequestDTO request;
  request.robot_id = value.get<std::string>("robot_id");
  request.group_id = value.get<std::string>("group_id");
  request.move_id = value.get<std::string>("move_id");
  request.base_frame = value.get<std::string>("base_frame");
  request.tcp_frame = value.get<std::string>("tcp_frame");
  request.start_state.joint_names =
      array_values<std::string>(value.get_child("start_state.joint_names"));
  request.start_state.positions_rad =
      array_values<double>(value.get_child("start_state.positions_rad"));
  if (const auto velocities =
          value.get_child_optional("start_state.velocities_rad_s")) {
    request.start_state.velocities_rad_s = array_values<double>(*velocities);
  }
  if (request.start_state.joint_names != expected_joint_names ||
      request.start_state.positions_rad.size() != expected_joint_names.size()) {
    throw std::invalid_argument(
        "captured PlanRequestDTO start state does not match model authority");
  }
  request.tolerances.joint_position_tol_rad =
      value.get<double>("tolerances.joint_position_tol_rad");
  request.tolerances.translation_tol_m =
      value.get<double>("tolerances.translation_tol_m");
  request.tolerances.rotation_tol_rad =
      value.get<double>("tolerances.rotation_tol_rad");
  request.speed_profile.velocity_scale =
      value.get<double>("speed_profile.velocity_scale");
  request.speed_profile.acceleration_scale =
      value.get<double>("speed_profile.acceleration_scale");
  request.speed_profile.max_tcp_linear_speed_mps = optional_json_double(
      value, "speed_profile.max_tcp_linear_speed_mps");
  request.planning_timeout_s = value.get<double>("planning_timeout_s");
  request.planning_attempt_index =
      value.get<std::uint32_t>("planning_attempt_index");
  request.contact_policy_id = value.get<std::string>("contact_policy_id", "");
  if (const auto limits =
          value.get_child_optional("per_joint_dynamics_limits")) {
    for (const auto& [joint_name, encoded] : *limits) {
      manyforge::core::JointDynamicsLimitDTO limit;
      limit.max_velocity_rad_s =
          optional_json_double(encoded, "max_velocity_rad_s");
      limit.max_acceleration_rad_s2 =
          optional_json_double(encoded, "max_acceleration_rad_s2");
      limit.max_jerk_rad_s3 = optional_json_double(encoded, "max_jerk_rad_s3");
      request.per_joint_dynamics_limits.emplace(joint_name, std::move(limit));
    }
  }
  request.scene_binding = replay_binding;
  return request;
}

void require_scene_edit(const manyforge::core::SceneEditResultDTO& result,
                        const std::string& action) {
  if (!result.success) {
    throw std::runtime_error(
        result.error ? result.error->message : "capture " + action + " failed");
  }
}

void configure_captured_world(NativeKernelBundle& bundle,
                              const ptree& capture,
                              const std::filesystem::path& repo_root) {
  if (capture.get<std::string>("schema_version") !=
      "manyforge.roboplan_transition_capture.v1") {
    throw std::invalid_argument("unsupported planning capture schema");
  }
  SetCollisionPolicyRequestDTO policy_request;
  policy_request.policy = captured_collision_policy(
      capture.get_child("scene.authority.configured_collision_policy"));
  require_scene_edit(bundle.scene_authority->set_collision_policy(policy_request),
                     "collision policy");
  const auto applied_policy =
      bundle.projection_backend->configured_collision_policy();
  if (applied_policy.policy_id != policy_request.policy.policy_id ||
      applied_policy.revision.revision_id !=
          policy_request.policy.revision.revision_id ||
      applied_policy.revision.content_hash !=
          policy_request.policy.revision.content_hash) {
    throw std::runtime_error(
        "captured collision policy was not preserved by replay setup");
  }
  SetRobotBasePoseRequestDTO base;
  base.robot_id = capture.get<std::string>("scene.robot.robot_id");
  base.pose_in_world = captured_pose(
      capture.get_child("scene.robot.pose_in_world"));
  require_scene_edit(bundle.scene_authority->set_robot_base_pose(base),
                     "robot base pose");

  for (const auto& [key, object] : capture.get_child("scene.objects")) {
    (void)key;
    AddObjectRequestDTO request;
    request.object_id = object.get<std::string>("object_id");
    request.frame_id = "universe";
    request.pose = captured_pose(object.get_child("pose_in_world"));
    const auto shape_type = object.get<std::string>("shape.type");
    if (shape_type == "mesh") {
      const auto scale =
          array_values<double>(object.get_child("shape.mesh_scale"));
      if (scale.size() != 3) {
        throw std::invalid_argument("captured mesh scale has invalid dimensions");
      }
      auto resource =
          std::filesystem::path(object.get<std::string>("shape.resource_uri"));
      if (!resource.is_absolute()) {
        resource = repo_root / resource;
      }
      request.shape = manyforge::core::CollisionShapeDTO{MeshShapeDTO{
          resource.string(), Vector3DTO{scale[0], scale[1], scale[2]}}};
    } else if (shape_type == "box") {
      const auto dimensions =
          array_values<double>(object.get_child("shape.dimensions_m"));
      if (dimensions.size() != 3) {
        throw std::invalid_argument("captured box has invalid dimensions");
      }
      request.shape = manyforge::core::CollisionShapeDTO{
          BoxShapeDTO{Vector3DTO{dimensions[0], dimensions[1], dimensions[2]}}};
    } else {
      throw std::invalid_argument("unsupported captured shape type: " +
                                  shape_type);
    }
    if (const auto support_contacts =
            object.get_child_optional("support_contacts")) {
      for (const auto& [contact_key, contact] : *support_contacts) {
        (void)contact_key;
        SupportContactDTO policy;
        policy.support_object_id =
            contact.get<std::string>("support_object_id");
        const auto mode =
            contact.get<std::string>("mode", "contact_release");
        if (mode == "contact_release") {
          policy.mode =
              manyforge::core::CollisionContactPolicyMode::kContactRelease;
        } else if (mode == "bounded_penetration") {
          policy.mode = manyforge::core::CollisionContactPolicyMode::
              kBoundedPenetration;
          policy.max_contact_penetration_m =
              contact.get<double>("max_contact_penetration_m");
          if (!std::isfinite(policy.max_contact_penetration_m) ||
              policy.max_contact_penetration_m < 0.0) {
            throw std::invalid_argument(
                "captured bounded support contact has invalid penetration bound");
          }
          request.support_contacts.push_back(std::move(policy));
          continue;
        } else if (mode == "contact_process") {
          policy.mode =
              manyforge::core::CollisionContactPolicyMode::kContactProcess;
          request.support_contacts.push_back(std::move(policy));
          continue;
        } else {
          throw std::invalid_argument(
              "captured support contact has unsupported mode: " + mode);
        }
        const auto evidence_mode = contact.get<std::string>(
            "release_evidence_mode", "signed_penetration");
        if (evidence_mode == "signed_penetration") {
          policy.release_evidence_mode =
              ContactReleaseEvidenceMode::kSignedPenetration;
        } else if (evidence_mode == "binary_contact") {
          policy.release_evidence_mode =
              ContactReleaseEvidenceMode::kBinaryContact;
        } else {
          throw std::invalid_argument(
              "captured support contact has unsupported evidence mode: " +
              evidence_mode);
        }
        policy.enforce_max_start_penetration = contact.get<bool>(
            "enforce_max_start_penetration",
            policy.release_evidence_mode ==
                ContactReleaseEvidenceMode::kSignedPenetration);
        policy.max_start_penetration_m =
            contact.get<double>("max_start_penetration_m", 0.0);
        policy.release_tolerance_m =
            contact.get<double>(
                "release_tolerance_m",
                policy.release_evidence_mode ==
                        ContactReleaseEvidenceMode::kSignedPenetration
                    ? 1e-5
                    : 0.0);
        policy.min_release_progress_m =
            contact.get<double>("min_release_progress_m", 0.0);
        policy.require_separation_by_end =
            contact.get<bool>("require_separation_by_end", true);
        policy.require_strictly_decreasing_penetration = contact.get<bool>(
            "require_strictly_decreasing_penetration",
            policy.release_evidence_mode ==
                ContactReleaseEvidenceMode::kSignedPenetration);
        policy.allow_recontact = contact.get<bool>("allow_recontact", false);
        if (policy.release_evidence_mode ==
            ContactReleaseEvidenceMode::kBinaryContact) {
          if (policy.enforce_max_start_penetration ||
              policy.max_start_penetration_m != 0.0 ||
              policy.release_tolerance_m != 0.0 ||
              policy.min_release_progress_m != 0.0 ||
              policy.require_strictly_decreasing_penetration) {
            throw std::invalid_argument(
                "captured binary support contact contains metric constraints");
          }
        }
        request.support_contacts.push_back(std::move(policy));
      }
    }
    require_scene_edit(bundle.scene_authority->add_object(request),
                       "object add");
  }

  for (const auto& [key, object] : capture.get_child("scene.objects")) {
    (void)key;
    if (!object.get<bool>("attached")) {
      continue;
    }
    AttachObjectRequestDTO request;
    request.object_id = object.get<std::string>("object_id");
    request.frame_id = object.get<std::string>("attached_to");
    request.touch_links =
        array_values<std::string>(object.get_child("touch_links"));
    require_scene_edit(bundle.scene_authority->attach_object(request),
                       "object attach");
  }
}

void configure_world(NativeKernelBundle& bundle,
                     const std::string& world_profile,
                     const std::optional<ptree>& capture,
                     const std::filesystem::path& repo_root) {
  if (world_profile == "primitive_sparse_clean") {
    AddObjectRequestDTO request;
    request.object_id = "rp5_far_box";
    request.frame_id = "universe";
    request.pose = identity_pose(3.0, 3.0, 3.0);
    request.shape = manyforge::core::CollisionShapeDTO{
        BoxShapeDTO{Vector3DTO{0.1, 0.1, 0.1}}};
    const auto result = bundle.scene_authority->add_object(request);
    if (!result.success) {
      throw std::runtime_error(
          result.error ? result.error->message : "failed to add fixture box");
    }
    return;
  }
  if (world_profile == "captured_authority_snapshot" && capture) {
    configure_captured_world(bundle, *capture, repo_root);
    return;
  }
  throw std::invalid_argument("unsupported planning world profile: " +
                              world_profile);
}

using ProbeSnapshotFn = bool (*)(std::uint64_t*, std::uint64_t*) noexcept;

class RecordingObserver final : public IRoboPlanPlanningPerformanceObserver {
 public:
  explicit RecordingObserver(std::size_t capacity) {
    samples_.reserve(capacity);
  }

  RoboPlanCollisionProbeSnapshot collision_probe_snapshot()
      const noexcept override {
    static const auto snapshot = reinterpret_cast<ProbeSnapshotFn>(
        dlsym(RTLD_DEFAULT, "manyforge_roboplan_collision_probe_snapshot"));
    RoboPlanCollisionProbeSnapshot result;
    if (snapshot == nullptr) {
      return result;
    }
    result.available =
        snapshot(&result.query_count, &result.query_duration_ns);
    return result;
  }

  RoboPlanAllocationProbeSnapshot allocation_probe_snapshot()
      const noexcept override {
    return {
        true,
        g_allocation_count.load(std::memory_order_relaxed),
        g_allocation_bytes.load(std::memory_order_relaxed),
    };
  }

  void record(const PlanRequestDTO&, const PlanResultDTO&,
              const RoboPlanPlanningPipelineMetrics& metrics) noexcept override {
    try {
      samples_.push_back(metrics);
    } catch (...) {
      complete_ = false;
    }
  }

  const std::vector<RoboPlanPlanningPipelineMetrics>& samples() const {
    return samples_;
  }
  bool complete() const { return complete_; }

 private:
  std::vector<RoboPlanPlanningPipelineMetrics> samples_;
  bool complete_ = true;
};

std::uint64_t thread_cpu_time_ns() {
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
    throw std::runtime_error("clock_gettime(CLOCK_THREAD_CPUTIME_ID) failed");
  }
  return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(value.tv_nsec);
}

double percentile(std::vector<std::uint64_t> values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double rank = p * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(rank);
  const auto upper = std::min(lower + 1, values.size() - 1);
  const double fraction = rank - static_cast<double>(lower);
  return static_cast<double>(values[lower]) * (1.0 - fraction) +
         static_cast<double>(values[upper]) * fraction;
}

double median_absolute_deviation(const std::vector<std::uint64_t>& values) {
  if (values.empty()) {
    return 0.0;
  }
  const double median = percentile(values, 0.5);
  std::vector<std::uint64_t> deviations;
  deviations.reserve(values.size());
  for (const auto value : values) {
    deviations.push_back(static_cast<std::uint64_t>(
        std::abs(static_cast<double>(value) - median)));
  }
  return percentile(std::move(deviations), 0.5);
}

bool trajectory_invariants_hold(
    const PlanRequestDTO& request, const PlanResultDTO& result,
    const manyforge::core::IRobotKinematics& kinematics) {
  if (!result.trajectory || result.error ||
      result.scene_revision.revision_id !=
          request.scene_binding.revision.revision_id ||
      result.scene_revision.content_hash !=
          request.scene_binding.revision.content_hash) {
    return false;
  }
  const auto& trajectory = *result.trajectory;
  if (trajectory.joint_names != request.start_state.joint_names ||
      trajectory.waypoints.empty()) {
    return false;
  }
  double previous_time = -1.0;
  for (const auto& waypoint : trajectory.waypoints) {
    if (!std::isfinite(waypoint.time_from_start_s) ||
        waypoint.time_from_start_s < previous_time ||
        waypoint.positions_rad.size() != trajectory.joint_names.size()) {
      return false;
    }
    for (const auto position : waypoint.positions_rad) {
      if (!std::isfinite(position)) {
        return false;
      }
    }
    previous_time = waypoint.time_from_start_s;
  }
  if (const auto* goal = std::get_if<JointGoalDTO>(&request.motion)) {
    if (goal->positions_rad.size() !=
        trajectory.waypoints.back().positions_rad.size()) {
      return false;
    }
    for (std::size_t index = 0; index < goal->positions_rad.size(); ++index) {
      if (std::abs(goal->positions_rad[index] -
                   trajectory.waypoints.back().positions_rad[index]) > 1e-9) {
        return false;
      }
    }
    return true;
  }
  const auto* pose_goal = std::get_if<PoseGoalDTO>(&request.motion);
  if (pose_goal == nullptr) {
    return false;
  }
  FkRequestDTO fk;
  fk.robot_state.joint_names = trajectory.joint_names;
  fk.robot_state.positions_rad = trajectory.waypoints.back().positions_rad;
  fk.target_frame = request.tcp_frame;
  fk.base_frame = request.base_frame;
  const auto resolved = kinematics.forward_kinematics(fk);
  if (!resolved.ok()) {
    return false;
  }
  const auto& actual = resolved.value();
  const auto& expected = pose_goal->goal;
  return pose_translation_distance(actual, expected) <=
             request.tolerances.translation_tol_m + 1e-9 &&
         quaternion_angular_distance(actual, expected) <=
             request.tolerances.rotation_tol_rad + 1e-9;
}

std::uint64_t trajectory_hash(const manyforge::core::TrajectoryDTO& trajectory) {
  std::uint64_t hash = 1469598103934665603ULL;
  auto mix = [&](const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
      hash ^= bytes[index];
      hash *= 1099511628211ULL;
    }
  };
  for (const auto& waypoint : trajectory.waypoints) {
    mix(&waypoint.time_from_start_s, sizeof(waypoint.time_from_start_s));
    for (const auto value : waypoint.positions_rad) {
      mix(&value, sizeof(value));
    }
    for (const auto value : waypoint.velocities_rad_s) {
      mix(&value, sizeof(value));
    }
    for (const auto value : waypoint.accelerations_rad_s2) {
      mix(&value, sizeof(value));
    }
  }
  return hash;
}

void append_latency_json(std::ostringstream& out,
                         const std::vector<std::uint64_t>& values) {
  out << "{\"p50\":" << percentile(values, 0.50)
      << ",\"p95\":" << percentile(values, 0.95)
      << ",\"p99\":null"
      << ",\"max\":" << percentile(values, 1.0)
      << ",\"mad\":" << median_absolute_deviation(values) << "}";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    ptree corpus;
    boost::property_tree::read_json(options.corpus_path.string(), corpus);
    auto fixture = load_fixture(corpus, options.fixture_id);
    auto model = load_model(corpus, fixture.model_id, options.repo_root);
    if (options.urdf_override.has_value() !=
        options.srdf_override.has_value()) {
      throw std::invalid_argument(
          "--urdf and --srdf must be provided together");
    }
    if (options.urdf_override) {
      model.urdf_path = *options.urdf_override;
      model.srdf_path = *options.srdf_override;
      if (!options.package_path_overrides.empty()) {
        model.package_paths = options.package_path_overrides;
      }
    } else if (!options.package_path_overrides.empty()) {
      throw std::invalid_argument(
          "--package-path requires --urdf and --srdf overrides");
    }
    std::optional<ptree> capture;
    if (fixture.capture_file) {
      capture.emplace();
      boost::property_tree::read_json(
          (options.corpus_path.parent_path() / *fixture.capture_file).string(),
          *capture);
      fixture.start = array_values<double>(
          capture->get_child("plan_request.start_state.positions_rad"));
      const auto captured_seed =
          capture->get<std::uint32_t>("planner_contract.rng_base_seed");
      if (captured_seed != options.rng_seed) {
        throw std::invalid_argument(
            "benchmark RNG seed does not match captured planner contract");
      }
    }
    auto bundle = create_bundle(model);

    auto joint_names = bundle->kinematics->group_joint_names(model.group_name);
    if (!joint_names.ok()) {
      throw std::runtime_error(joint_names.error().message);
    }
    if (joint_names.value().size() != fixture.start.size() ||
        (!capture && fixture.start.size() != fixture.goal.size())) {
      throw std::runtime_error("fixture joint vector does not match model group");
    }
    const auto state_seed = bundle->execution_state_sink->apply_group_joint_positions(
        model.group_name, fixture.start, ExecutionStateUpdateMode::kBlocking);
    if (!state_seed.applied()) {
      throw std::runtime_error(
          "failed to seed the captured planning projection: " +
          std::string(manyforge::core::execution_state_sink_status_reason(
              state_seed.status)));
    }
    // Attach/reparent preserves world pose by reading the current robot frame.
    // Seed the captured joints first so the reconstructed object-to-tool
    // transform is identical to the live authority snapshot.
    configure_world(*bundle, fixture.world_profile, capture,
                    options.repo_root);

    const std::size_t total_repetitions = static_cast<std::size_t>(
        options.warmup_repetitions + options.measured_repetitions);
    auto observer = std::make_shared<RecordingObserver>(total_repetitions);
    const manyforge::core::ExecutionKernelConfig production_kernel_config;
    auto rrt_options = production_rrt_options(
        model.group_name,
        production_kernel_config.planner_max_planning_time_s);
    switch (options.planner_policy) {
      case Options::PlannerPolicy::kCompatibility:
        break;
      case Options::PlannerPolicy::kRrtStar:
        rrt_options.rrt_star = true;
        rrt_options.fast_return = false;
        break;
      case Options::PlannerPolicy::kBudgetSpending:
        rrt_options.rrt_star = false;
        rrt_options.fast_return = false;
        break;
    }
    const auto planning_ik_options =
        production_planning_ik_options(model.group_name);
    const auto single_shot_ik_options = production_single_shot_ik_options(
        model.group_name,
        production_kernel_config.single_shot_ik_timeout_s);
    RoboPlanPlannerBackend planner(
        bundle->scene_projection, bundle->scene, rrt_options,
        bundle->scene_mutex, planning_ik_options, single_shot_ik_options,
        options.rng_seed, observer);

    std::vector<std::uint64_t> wall_ns;
    std::vector<std::uint64_t> cpu_ns;
    std::vector<std::uint64_t> allocations;
    std::vector<std::uint64_t> allocation_bytes;
    std::vector<std::uint64_t> hashes;
    std::vector<std::uint64_t> target_resolution_ns;
    bool invariants_passed = true;
    bool outcomes_passed = true;
    wall_ns.reserve(options.measured_repetitions);
    cpu_ns.reserve(options.measured_repetitions);
    allocations.reserve(options.measured_repetitions);
    allocation_bytes.reserve(options.measured_repetitions);
    hashes.reserve(options.measured_repetitions);
    target_resolution_ns.reserve(options.measured_repetitions);

    for (std::size_t repetition = 0; repetition < total_repetitions;
         ++repetition) {
      const std::string retention_id =
          "rp5_" + fixture.id + "_" + std::to_string(repetition);
      auto retained = bundle->scene_authority->retain_scene_snapshot_lease_inventory(
          retention_id);
      if (!retained || !retained->bundle.world) {
        throw std::runtime_error("failed to retain planning fixture revision");
      }
      PlanRequestDTO request;
      SceneRevisionBindingDTO replay_binding;
      replay_binding.revision = retained->bundle.world->revision;
      replay_binding.projection_lease_id = retained->retention_id;
      if (capture) {
        request = captured_plan_request(
            *capture, joint_names.value(), replay_binding);
        if (request.robot_id != model.model_name ||
            request.group_id != model.group_name ||
            request.base_frame != model.base_frame ||
            request.tcp_frame != model.tcp_frame) {
          throw std::runtime_error(
              "captured PlanRequestDTO identity does not match model authority");
        }
      } else {
        request.robot_id = model.model_name;
        request.group_id = model.group_name;
        request.move_id = fixture.id;
        request.base_frame = model.base_frame;
        request.tcp_frame = model.tcp_frame;
        request.start_state.joint_names = joint_names.value();
        request.start_state.positions_rad = fixture.start;
        JointGoalDTO goal;
        goal.joint_names = joint_names.value();
        goal.positions_rad = fixture.goal;
        request.motion = std::move(goal);
        request.tolerances = {0.05, 0.005, 0.05};
        request.speed_profile = {0.5, 0.5, std::nullopt};
        request.planning_timeout_s = 10.0;
        request.scene_binding = replay_binding;
      }

      g_allocation_count.store(0, std::memory_order_relaxed);
      g_allocation_bytes.store(0, std::memory_order_relaxed);
      const auto wall_started = std::chrono::steady_clock::now();
      const auto cpu_started = thread_cpu_time_ns();
      g_count_allocations.store(repetition >=
                                    static_cast<std::size_t>(
                                        options.warmup_repetitions),
                                std::memory_order_release);
      std::uint64_t target_resolution_duration_ns = 0;
      if (capture) {
        const auto resolution_started = std::chrono::steady_clock::now();
        PoseGoalDTO goal;
        goal.goal = resolve_captured_pose_goal(
            *bundle, *capture, request.base_frame, request.scene_binding);
        const auto captured_goal = captured_pose(
            capture->get_child("plan_request.motion.pose"));
        if (pose_translation_distance(goal.goal, captured_goal) > 1e-8 ||
            quaternion_angular_distance(goal.goal, captured_goal) > 1e-8) {
          throw std::runtime_error(
              "captured PlanRequestDTO goal disagrees with reference replay");
        }
        request.motion = std::move(goal);
        target_resolution_duration_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - resolution_started)
                .count());
      }
      const auto result = planner.plan(request);
      g_count_allocations.store(false, std::memory_order_release);
      const auto cpu_finished = thread_cpu_time_ns();
      const auto wall_finished = std::chrono::steady_clock::now();
      bundle->scene_authority->release_scene_snapshot_lease_inventory(
          retained->retention_id);

      const bool succeeded = result.trajectory.has_value() && !result.error;
      outcomes_passed &= succeeded == fixture.expected_success;
      invariants_passed &=
          !succeeded ||
          trajectory_invariants_hold(request, result, *bundle->kinematics);
      if (repetition <
          static_cast<std::size_t>(options.warmup_repetitions)) {
        continue;
      }
      wall_ns.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              wall_finished - wall_started)
              .count()));
      cpu_ns.push_back(cpu_finished - cpu_started);
      allocations.push_back(
          g_allocation_count.load(std::memory_order_relaxed));
      allocation_bytes.push_back(
          g_allocation_bytes.load(std::memory_order_relaxed));
      if (capture) {
        target_resolution_ns.push_back(target_resolution_duration_ns);
      }
      if (result.trajectory) {
        hashes.push_back(trajectory_hash(*result.trajectory));
      }
    }

    if (!observer->complete() ||
        observer->samples().size() != total_repetitions) {
      throw std::runtime_error("planner observer did not retain every sample");
    }
    const auto measured_begin = observer->samples().begin() +
                                options.warmup_repetitions;
    std::map<std::string, std::vector<std::uint64_t>> stage_durations;
    std::map<std::string, std::vector<std::uint64_t>> stage_collision_counts;
    std::map<std::string, std::vector<std::uint64_t>>
        stage_collision_duration;
    std::map<std::string, std::vector<std::uint64_t>> stage_allocation_counts;
    std::map<std::string, std::vector<std::uint64_t>> stage_allocation_bytes;
    bool metrics_complete = true;
    bool collision_probe_available = true;
    std::uint64_t total_collision_query_count = 0;
    std::uint64_t total_collision_query_duration_ns = 0;
    std::optional<int> shortcut_rng_seed_argument;
    for (auto sample = measured_begin; sample != observer->samples().end();
         ++sample) {
      metrics_complete &= sample->measurement_complete;
      if (!shortcut_rng_seed_argument.has_value()) {
        shortcut_rng_seed_argument = sample->shortcut_rng_seed_argument;
      }
      metrics_complete &= sample->shortcut_rng_seed_argument.has_value() &&
                          sample->shortcut_rng_seed_argument ==
                              shortcut_rng_seed_argument;
      collision_probe_available &= sample->collision_probe_available;
      total_collision_query_count += sample->total_collision_query_count;
      total_collision_query_duration_ns +=
          sample->total_collision_query_duration_ns;
      for (const auto& [stage_name, stage] : sample->stages) {
        stage_durations[stage_name].push_back(stage.duration_ns);
        stage_collision_counts[stage_name].push_back(
            stage.collision_query_count);
        stage_collision_duration[stage_name].push_back(
            stage.collision_query_duration_ns);
        stage_allocation_counts[stage_name].push_back(stage.allocation_count);
        stage_allocation_bytes[stage_name].push_back(stage.allocation_bytes);
      }
    }
    if (capture) {
      stage_durations["target_resolution"] = target_resolution_ns;
      stage_collision_counts["target_resolution"].assign(
          target_resolution_ns.size(), 0);
      stage_collision_duration["target_resolution"].assign(
          target_resolution_ns.size(), 0);
      stage_allocation_counts["target_resolution"].assign(
          target_resolution_ns.size(), 0);
      stage_allocation_bytes["target_resolution"].assign(
          target_resolution_ns.size(), 0);
    }
    if (collision_probe_available) {
      const auto rrt = stage_collision_counts.find("rrt");
      if (rrt == stage_collision_counts.end() ||
          std::none_of(
              rrt->second.begin(), rrt->second.end(),
              [](const std::uint64_t count) { return count > 0; })) {
        throw std::runtime_error(
            "collision probe observed no RRT collision queries; verify that "
            "the active RoboPlan collision owner is interposed");
      }
    }
    std::sort(hashes.begin(), hashes.end());
    const auto distinct_hashes =
        std::distance(hashes.begin(), std::unique(hashes.begin(), hashes.end()));
    const bool deterministic = distinct_hashes <= 1;
    const std::optional<std::uint64_t> deterministic_hash =
        hashes.empty() ? std::nullopt
                       : std::make_optional(hashes.front());
    const bool passed = outcomes_passed && invariants_passed &&
                        metrics_complete && deterministic;

    std::ostringstream out;
    out << std::setprecision(17)
        << "{\"record_type\":\"planning_pipeline\""
        << ",\"fixture_id\":\"" << json_escape(fixture.id) << "\""
        << ",\"passed\":" << (passed ? "true" : "false")
        << ",\"sample_count\":" << options.measured_repetitions
        << ",\"warmup_count\":" << options.warmup_repetitions
        << ",\"rng_seed\":" << options.rng_seed
        << ",\"planner_policy\":\""
        << planner_policy_name(options.planner_policy) << "\""
        << ",\"shortcut_rng_seed_argument\":"
        << shortcut_rng_seed_argument.value_or(0)
        << ",\"planner_options\":{"
        << "\"rrt_max_planning_time_s\":"
        << rrt_options.max_planning_time
        << ",\"rrt_max_nodes\":" << rrt_options.max_nodes
        << ",\"rrt_star\":"
        << (rrt_options.rrt_star ? "true" : "false")
        << ",\"fast_return\":"
        << (rrt_options.fast_return ? "true" : "false")
        << ",\"ik_step_size\":" << planning_ik_options.step_size
        << ",\"ik_max_restarts\":" << planning_ik_options.max_restarts
        << ",\"ik_max_time_s\":" << planning_ik_options.max_time
        << ",\"single_shot_ik_max_time_s\":"
        << single_shot_ik_options.max_time << "}"
        << ",\"capture_semantic_sha256\":";
    if (capture) {
      out << '\"'
          << json_escape(capture->get<std::string>("semantic_sha256"))
          << '\"';
    } else {
      out << "null";
    }
    out
        << ",\"latency_ns\":";
    append_latency_json(out, wall_ns);
    out << ",\"cpu_time_ns\":";
    append_latency_json(out, cpu_ns);
    out << ",\"allocation_count\":";
    append_latency_json(out, allocations);
    out << ",\"allocation_bytes\":";
    append_latency_json(out, allocation_bytes);
    out << ",\"invariants_passed\":"
        << (invariants_passed ? "true" : "false")
        << ",\"outcomes_passed\":"
        << (outcomes_passed ? "true" : "false")
        << ",\"deterministic_trajectory\":"
        << (deterministic ? "true" : "false")
        << ",\"distinct_trajectory_hashes\":" << distinct_hashes
        << ",\"trajectory_hash_hex\":";
    if (deterministic_hash) {
      out << '\"' << std::hex << *deterministic_hash << std::dec << '\"';
    } else {
      out << "null";
    }
    out
        << ",\"collision_probe_available\":"
        << (collision_probe_available ? "true" : "false")
        << ",\"total_collision_query_count\":"
        << total_collision_query_count
        << ",\"total_collision_query_duration_ns\":"
        << total_collision_query_duration_ns
        << ",\"stages\":{";
    bool first_stage = true;
    for (const auto& [stage_name, values] : stage_durations) {
      if (!first_stage) {
        out << ',';
      }
      first_stage = false;
      out << '\"' << json_escape(stage_name) << "\":{\"latency_ns\":";
      append_latency_json(out, values);
      out << ",\"collision_query_count\":";
      append_latency_json(out, stage_collision_counts[stage_name]);
      out << ",\"collision_query_duration_ns\":";
      append_latency_json(out, stage_collision_duration[stage_name]);
      out << ",\"allocation_count\":";
      append_latency_json(out, stage_allocation_counts[stage_name]);
      out << ",\"allocation_bytes\":";
      append_latency_json(out, stage_allocation_bytes[stage_name]);
      out << '}';
    }
    out << "}}";
    std::cout << out.str() << '\n';
    return passed ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "perf_roboplan_planning_pipeline: " << error.what() << '\n';
    return 2;
  }
}
