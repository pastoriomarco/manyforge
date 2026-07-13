#include <manyforge_collision/manyforge_collision_engine.hpp>

#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <roboplan_example_models/resources.hpp>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <Eigen/Geometry>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
#include <time.h>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::uint64_t> g_allocation_count{0};

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
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* pointer = allocate_bytes(size)) {
    return pointer;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
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
using manyforge::collision::ManyForgeCollisionEngine;
using manyforge::core::CollisionCancellationToken;
using manyforge::core::CollisionEngineStatus;
using manyforge::core::CollisionGeometryDescriptor;
using manyforge::core::CollisionGeometryKind;
using manyforge::core::CollisionOwnerKind;
using manyforge::core::CollisionPairId;
using manyforge::core::CollisionPolicySnapshot;
using manyforge::core::CollisionQuery;
using manyforge::core::CollisionResult;
using manyforge::core::CollisionSurface;
using manyforge::core::CollisionWorldBody;
using manyforge::core::CollisionWorldSnapshot;
using manyforge::core::CollisionWorldSnapshotBuilder;
using manyforge::core::FkRequestDTO;
using manyforge::core::Pose3DTO;
using manyforge::core::RobotStateDTO;
using manyforge::core::RuntimeCollisionQueryRequest;
using manyforge::core::RuntimeCollisionQueryStatus;
using manyforge::core::SetRobotBasePoseRequestDTO;
using manyforge::core::SupportContactPolicyEntry;
using manyforge::core::Vector3DTO;
using manyforge::roboplan_backend::NativeKernelBundle;
using manyforge::roboplan_backend::create_native_kernel;

constexpr double kPi = 3.14159265358979323846;

struct Options {
  std::filesystem::path repo_root = "/workspace";
  std::filesystem::path corpus_path;
  std::string fixture_id;
  std::string query_class;
  int warmup_queries = 1000;
  int measured_queries = 10000;
  std::optional<std::string> urdf_override;
  std::optional<std::string> srdf_override;
  std::vector<std::filesystem::path> package_path_overrides;
};

struct Model {
  std::string source;
  std::string model_name;
  std::string group_name;
  std::string base_frame;
  std::string tcp_frame;
  std::string urdf_path;
  std::string srdf_path;
  std::vector<std::filesystem::path> package_paths;
  std::vector<std::string> joint_names;
  Pose3DTO robot_base_world_pose;
};

struct Fixture {
  std::string id;
  std::string model_id;
  std::string world_profile;
  std::vector<double> robot_state;
  bool expected_collision = false;
  bool support_contact_policy = false;
};

template <typename T>
std::vector<T> array_values(const ptree& tree) {
  std::vector<T> values;
  for (const auto& [key, value] : tree) {
    (void)key;
    values.push_back(value.get_value<T>());
  }
  return values;
}

Pose3DTO pose_from_rpy(const std::vector<double>& position,
                      const std::vector<double>& rpy_deg) {
  if (position.size() != 3 || rpy_deg.size() != 3) {
    throw std::invalid_argument("pose requires three position and RPY values");
  }
  const double roll = rpy_deg[0] * kPi / 180.0;
  const double pitch = rpy_deg[1] * kPi / 180.0;
  const double yaw = rpy_deg[2] * kPi / 180.0;
  const double cr = std::cos(roll * 0.5);
  const double sr = std::sin(roll * 0.5);
  const double cp = std::cos(pitch * 0.5);
  const double sp = std::sin(pitch * 0.5);
  const double cy = std::cos(yaw * 0.5);
  const double sy = std::sin(yaw * 0.5);
  Pose3DTO result;
  result.position_m = Vector3DTO{position[0], position[1], position[2]};
  result.orientation.w = cr * cp * cy + sr * sp * sy;
  result.orientation.x = sr * cp * cy - cr * sp * sy;
  result.orientation.y = cr * sp * cy + sr * cp * sy;
  result.orientation.z = cr * cp * sy - sr * sp * cy;
  return result;
}

Pose3DTO compose_pose(const Pose3DTO& parent, const Pose3DTO& child) {
  const Eigen::Quaterniond parent_q(
      parent.orientation.w, parent.orientation.x, parent.orientation.y,
      parent.orientation.z);
  const Eigen::Quaterniond child_q(
      child.orientation.w, child.orientation.x, child.orientation.y,
      child.orientation.z);
  const Eigen::Vector3d child_translation(
      child.position_m.x_m, child.position_m.y_m, child.position_m.z_m);
  const Eigen::Vector3d parent_translation(
      parent.position_m.x_m, parent.position_m.y_m, parent.position_m.z_m);
  const Eigen::Vector3d translation =
      parent_translation + parent_q * child_translation;
  const Eigen::Quaterniond orientation = parent_q * child_q;
  Pose3DTO result;
  result.position_m = {translation.x(), translation.y(), translation.z()};
  result.orientation = {orientation.x(), orientation.y(), orientation.z(),
                        orientation.w()};
  return result;
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
    } else if (arg == "--query-class") {
      options.query_class = next();
    } else if (arg == "--warmup") {
      options.warmup_queries = std::stoi(next());
    } else if (arg == "--queries") {
      options.measured_queries = std::stoi(next());
    } else if (arg == "--urdf") {
      options.urdf_override = next();
    } else if (arg == "--srdf") {
      options.srdf_override = next();
    } else if (arg == "--package-path") {
      options.package_path_overrides.emplace_back(next());
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }
  if (options.corpus_path.empty() || options.fixture_id.empty() ||
      options.query_class.empty()) {
    throw std::invalid_argument(
        "--corpus, --fixture, and --query-class are required");
  }
  if (options.warmup_queries < 0 || options.measured_queries < 1) {
    throw std::invalid_argument("invalid query count");
  }
  return options;
}

Fixture load_fixture(const ptree& root, const std::string& fixture_id,
                     const std::string& query_class) {
  for (const auto& [key, candidate] : root.get_child("collision_fixtures")) {
    (void)key;
    if (candidate.get<std::string>("id") != fixture_id) {
      continue;
    }
    const auto query_classes =
        array_values<std::string>(candidate.get_child("query_classes"));
    if (std::find(query_classes.begin(), query_classes.end(), query_class) ==
        query_classes.end()) {
      throw std::invalid_argument("query class is not declared by fixture");
    }
    Fixture fixture;
    fixture.id = fixture_id;
    fixture.model_id = candidate.get<std::string>("model");
    fixture.world_profile = candidate.get<std::string>("world_profile");
    fixture.robot_state =
        array_values<double>(candidate.get_child("robot_state_rad"));
    const auto decision = candidate.get<std::string>("expected.decision");
    fixture.expected_collision =
        decision == "collision" || decision == "support_contact_policy";
    fixture.support_contact_policy = decision == "support_contact_policy";
    return fixture;
  }
  throw std::invalid_argument("unknown collision fixture: " + fixture_id);
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
  model.joint_names = array_values<std::string>(value.get_child("joint_names"));
  const auto& base_pose = value.get_child("robot_base_world_pose");
  model.robot_base_world_pose = pose_from_rpy(
      array_values<double>(base_pose.get_child("position_m")),
      array_values<double>(base_pose.get_child("orientation_rpy_deg")));
  if (model.source == "roboplan_example_models") {
    const auto models_dir = roboplan::example_models::get_package_models_dir();
    const auto share_dir = roboplan::example_models::get_package_share_dir();
    if (model.model_name != "ur5_gripper") {
      throw std::invalid_argument("unknown RoboPlan example model");
    }
    model.urdf_path =
        (models_dir / "ur_robot_model/ur5_gripper.urdf").string();
    model.srdf_path =
        (models_dir / "ur_robot_model/ur5_gripper.srdf").string();
    model.package_paths.push_back(share_dir);
  } else {
    model.urdf_path =
        (repo_root / value.get<std::string>("urdf_path")).string();
    model.srdf_path =
        (repo_root / value.get<std::string>("srdf_path")).string();
    for (const auto& package : value.get_child("package_paths")) {
      model.package_paths.push_back(
          repo_root / package.second.get_value<std::string>());
    }
  }
  return model;
}

CollisionWorldBody box_body(std::string id, const Pose3DTO& pose,
                            const Vector3DTO& dimensions) {
  CollisionWorldBody body;
  body.body_id = "object/" + id + "/shape/0";
  body.owner_kind = CollisionOwnerKind::kDetachedObject;
  body.frame_id = "universe";
  body.pose = pose;
  CollisionGeometryDescriptor geometry;
  geometry.body_id = body.body_id;
  geometry.owner_kind = body.owner_kind;
  geometry.geometry_kind = CollisionGeometryKind::kBox;
  geometry.local_pose.orientation.w = 1.0;
  geometry.box_dimensions_m = dimensions;
  body.geometries.push_back(std::move(geometry));
  return body;
}

CollisionWorldBody mesh_body(const std::filesystem::path& repo_root,
                             std::string id, const std::string& relative_uri,
                             const Pose3DTO& pose, const Vector3DTO& scale) {
  CollisionWorldBody body;
  body.body_id = "object/" + id + "/shape/0";
  body.owner_kind = CollisionOwnerKind::kDetachedObject;
  body.frame_id = "universe";
  body.pose = pose;
  CollisionGeometryDescriptor geometry;
  geometry.body_id = body.body_id;
  geometry.owner_kind = body.owner_kind;
  geometry.geometry_kind = CollisionGeometryKind::kTriangleMesh;
  geometry.local_pose.orientation.w = 1.0;
  geometry.mesh_scale = scale;
  geometry.source_uri = (repo_root / relative_uri).string();
  geometry.source_hash = relative_uri;
  geometry.quality_labels = {"mesh", "collision_mesh"};
  body.geometries.push_back(std::move(geometry));
  return body;
}

CollisionWorldBody attached_box_body(
    const std::string& robot_id, const std::string& object_id,
    const std::string& parent_link, const Pose3DTO& local_pose,
    const Vector3DTO& dimensions) {
  CollisionWorldBody body;
  body.body_id =
      "robot/" + robot_id + "/attached/" + object_id + "/shape/0";
  body.owner_kind = CollisionOwnerKind::kAttachedObject;
  body.parent_link = "robot/" + robot_id + "/link/" + parent_link;
  body.frame_id = *body.parent_link;
  body.pose = local_pose;
  CollisionGeometryDescriptor geometry;
  geometry.body_id = body.body_id;
  geometry.owner_kind = body.owner_kind;
  geometry.geometry_kind = CollisionGeometryKind::kBox;
  geometry.local_pose.orientation.w = 1.0;
  geometry.box_dimensions_m = dimensions;
  body.geometries.push_back(std::move(geometry));
  return body;
}

CollisionWorldSnapshot build_snapshot(const CollisionWorldSnapshot& initial,
                                      const std::string& world_profile,
                                      const std::filesystem::path& repo_root,
                                      const std::string& robot_id,
                                      const std::string& tcp_frame,
                                      const std::optional<Pose3DTO>& tcp_world_pose,
                                      std::uint64_t revision = 1) {
  CollisionWorldSnapshotBuilder builder;
  builder.set_publisher_id("rp5_deterministic_corpus")
      .set_revision_id(revision)
      .set_emitted_at_ns(1);
  for (const auto& frame : initial.robot_model_frames) {
    builder.add_robot_model_frame(frame);
  }
  for (const auto& body : initial.bodies) {
    builder.add_body(body);
  }
  if (world_profile == "primitive_sparse_clean") {
    builder.add_body(box_body(
        "far_box", pose_from_rpy({3.0, 3.0, 3.0}, {0.0, 0.0, 0.0}),
        Vector3DTO{0.1, 0.1, 0.1}));
  } else if (world_profile == "primitive_robot_intersection") {
    builder.add_body(box_body(
        "intersection", pose_from_rpy({0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}),
        Vector3DTO{20.0, 20.0, 20.0}));
  } else if (world_profile == "primitive_dense_clean") {
    for (int index = 0; index < 64; ++index) {
      builder.add_body(box_body(
          "dense_" + std::to_string(index),
          pose_from_rpy(
              {3.0 + 0.05 * static_cast<double>(index % 8),
               2.5 + 0.05 * static_cast<double>((index / 8) % 8),
               2.0 + 0.05 * static_cast<double>(index / 64)},
              {0.0, 0.0, 0.0}),
          Vector3DTO{0.03, 0.03, 0.03}));
    }
  } else if (world_profile == "support_contact") {
    // One detached object begins in bounded penetration with its support. The
    // policy below makes this exact static/static pair query-relevant so the
    // distance row measures signed support-contact diagnostics rather than an
    // unrelated free-space query.
    if (!tcp_world_pose.has_value()) {
      throw std::invalid_argument(
          "support-contact profile requires the fixture TCP world pose");
    }
    Pose3DTO attached_local;
    attached_local.orientation.w = 1.0;
    builder.add_body(attached_box_body(
        robot_id, "supported_object", tcp_frame, attached_local,
        Vector3DTO{0.05, 0.05, 0.05}));
    Pose3DTO support_offset;
    support_offset.position_m.z_m = 0.025;
    support_offset.orientation.w = 1.0;
    builder.add_body(box_body("support_fixture",
                              compose_pose(*tcp_world_pose, support_offset),
                              Vector3DTO{0.30, 0.30, 0.04}));
  } else if (world_profile == "isaac_light_mesh") {
    const std::string base = "examples/isaac_manipulator_scene_light/meshes/";
    builder.add_body(mesh_body(
        repo_root, "isaac_table_low_327",
        base + "background__table_low_327_collider_cooked.obj",
        pose_from_rpy({0.0, 0.0, -0.7695999827980996}, {0.0, 0.0, 0.0}),
        Vector3DTO{1.0, 1.5, 1.149999976}));
    builder.add_body(mesh_body(
        repo_root, "isaac_cheese_box",
        base + "mac_n_cheese_centered_collider_cooked.obj",
        pose_from_rpy({-0.455038, 0.076595, 0.087827},
                      {92.340746, -1.76826, -90.093258}),
        Vector3DTO{1.0, 1.0, 1.0}));
    builder.add_body(mesh_body(
        repo_root, "isaac_soup_can",
        base + "World__soup_can_collider_cooked.obj",
        pose_from_rpy({0.085370734333992, -0.004780956543982029,
                       0.05248536914587021},
                      {89.988004, 0.106372, 173.509839}),
        Vector3DTO{1.0, 1.0, 1.0}));
    builder.add_body(mesh_body(
        repo_root, "isaac_small_KLT",
        base + "World__small_KLT_collider_primitives.obj",
        pose_from_rpy({-0.561839, -0.367252, 0.073459},
                      {-0.000076, 0.162992, 0.006698}),
        Vector3DTO{1.0, 1.0, 1.0}));
    builder.add_body(mesh_body(
        repo_root, "isaac_small_KLT_01",
        base + "World__small_KLT_01_collider_primitives.obj",
        pose_from_rpy({0.27161648869514465, -0.35853588581085205,
                       0.07468909025192261},
                      {-0.000021, -0.104678, -0.000040}),
        Vector3DTO{1.0, 1.0, 1.0}));
  } else {
    throw std::invalid_argument("unsupported collision world profile: " +
                                world_profile);
  }
  auto result = builder.build();
  if (!result.ok()) {
    throw std::runtime_error(result.error().message);
  }
  return std::move(result.value());
}

std::shared_ptr<NativeKernelBundle> create_bundle(const Model& model) {
  if (model.source == "roboplan_example_models") {
    return create_native_kernel(model.model_name, model.group_name);
  }
  std::vector<std::string> package_paths;
  package_paths.reserve(model.package_paths.size());
  for (const auto& path : model.package_paths) {
    package_paths.push_back(path.string());
  }
  return create_native_kernel(model.model_name, model.group_name,
                              model.urdf_path, model.srdf_path,
                              package_paths);
}

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

double mad(const std::vector<std::uint64_t>& values) {
  const double median = percentile(values, 0.5);
  std::vector<std::uint64_t> deviations;
  deviations.reserve(values.size());
  for (const auto value : values) {
    deviations.push_back(static_cast<std::uint64_t>(
        std::abs(static_cast<double>(value) - median)));
  }
  return percentile(std::move(deviations), 0.5);
}

void append_latency(std::ostringstream& out,
                    const std::vector<std::uint64_t>& values) {
  out << "{\"p50\":" << percentile(values, 0.50)
      << ",\"p95\":" << percentile(values, 0.95)
      << ",\"p99\":" << percentile(values, 0.99)
      << ",\"max\":" << percentile(values, 1.0)
      << ",\"mad\":" << mad(values) << "}";
}

std::string json_escape(const std::string& value) {
  std::ostringstream out;
  for (const char c : value) {
    if (c == '\\' || c == '"') {
      out << '\\';
    }
    out << c;
  }
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    ptree corpus;
    boost::property_tree::read_json(options.corpus_path.string(), corpus);
    const auto fixture =
        load_fixture(corpus, options.fixture_id, options.query_class);
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
    if (model.joint_names.size() != fixture.robot_state.size()) {
      throw std::runtime_error("fixture robot state does not match joint order");
    }
    auto bundle = create_bundle(model);
    if (!bundle || !bundle->scene_authority) {
      throw std::runtime_error("failed to create corpus scene authority");
    }
    SetRobotBasePoseRequestDTO base_pose;
    base_pose.robot_id = model.model_name;
    base_pose.pose_in_world = model.robot_base_world_pose;
    const auto base_result =
        bundle->scene_authority->set_robot_base_pose(base_pose);
    if (!base_result.success) {
      throw std::runtime_error(
          base_result.error ? base_result.error->message
                            : "failed to set corpus robot base pose");
    }
    const auto initial_snapshot = bundle->scene_authority->snapshot();
    if (!initial_snapshot) {
      throw std::runtime_error("corpus scene authority returned no snapshot");
    }
    ManyForgeCollisionEngine engine(
        model.model_name, model.urdf_path, model.srdf_path,
        model.package_paths, model.group_name);
    std::optional<Pose3DTO> tcp_world_pose;
    if (fixture.support_contact_policy) {
      FkRequestDTO request;
      request.robot_state.joint_names = model.joint_names;
      request.robot_state.positions_rad = fixture.robot_state;
      request.target_frame = model.tcp_frame;
      request.base_frame = model.base_frame;
      auto result = bundle->kinematics->forward_kinematics(request);
      if (!result.ok()) {
        throw std::runtime_error(result.error().message);
      }
      tcp_world_pose = compose_pose(model.robot_base_world_pose, result.value());
    }
    const auto snapshot =
        build_snapshot(*initial_snapshot, fixture.world_profile,
                       options.repo_root, model.model_name, model.tcp_frame,
                       tcp_world_pose);
    CollisionPolicySnapshot policy;
    policy.policy_id = "rp5_free_space";
    policy.revision.revision_id = 1;
    policy.revision.content_hash = "rp5_free_space_v1";
    if (fixture.support_contact_policy) {
      policy.policy_id = "rp5_support_contact_release";
      policy.revision.content_hash = "rp5_support_contact_release_v1";
      SupportContactPolicyEntry entry;
      entry.pair = {
          "object/support_fixture/shape/0",
          "robot/" + model.model_name +
              "/attached/supported_object/shape/0"};
      entry.max_start_penetration_m = 0.03;
      entry.release_tolerance_m = 0.001;
      policy.support_contacts.push_back(std::move(entry));
    }
    CollisionCancellationToken cancel;

    RobotStateDTO state;
    state.joint_names = model.joint_names;
    state.positions_rad = fixture.robot_state;
    CollisionQuery query;
    query.query_id = options.fixture_id + "_" + options.query_class;
    query.surface = options.query_class.rfind("runtime_", 0) == 0
                        ? CollisionSurface::kObservedRuntimeMonitor
                        : CollisionSurface::kPreExecutionValidation;
    query.robot_state = state;
    query.collision_world_revision = snapshot.revision;
    query.policy_revision = policy.revision.content_hash;
    if (options.query_class == "exact_distance" ||
        options.query_class == "runtime_distance") {
      query.required_capabilities = {"body_pair_only", "distance",
                                     "penetration_depth"};
    } else {
      query.required_capabilities = {"body_pair_only"};
    }

    const bool runtime = options.query_class.rfind("runtime_", 0) == 0;
    if (runtime) {
      const auto prepared = engine.prepare_runtime_query(model.joint_names);
      const auto snapshot_prepared = engine.prepare_runtime_snapshot(snapshot);
      if (prepared != manyforge::core::RuntimeCollisionQueryPrepareStatus::kReady ||
          snapshot_prepared !=
              manyforge::core::RuntimeCollisionQueryPrepareStatus::kReady) {
        throw std::runtime_error("runtime query preparation failed");
      }
    }

    std::vector<std::uint64_t> wall_ns;
    std::vector<std::uint64_t> cpu_ns;
    std::vector<std::uint64_t> allocations;
    wall_ns.reserve(options.measured_queries);
    cpu_ns.reserve(options.measured_queries);
    allocations.reserve(options.measured_queries);
    bool outcome_matches = true;
    std::size_t pair_count = 0;
    std::size_t clearance_count = 0;
    std::optional<double> min_clearance;
    const CollisionPairId support_pair =
        fixture.support_contact_policy
            ? policy.support_contacts.front().pair
            : CollisionPairId{};
    std::optional<double> support_clearance;
    const int total_queries = options.warmup_queries + options.measured_queries;
    for (int index = 0; index < total_queries; ++index) {
      const bool measured = index >= options.warmup_queries;
      g_allocation_count.store(0, std::memory_order_relaxed);
      const auto wall_started = std::chrono::steady_clock::now();
      const auto cpu_started = thread_cpu_time_ns();
      g_count_allocations.store(measured, std::memory_order_release);
      if (runtime) {
        RuntimeCollisionQueryRequest request;
        request.positions_rad = state.positions_rad;
        request.state_time_ns = 1;
        request.timeout_budget_ns = 1'000'000'000;
        request.require_distance = options.query_class == "runtime_distance";
        const auto result = engine.check_runtime(snapshot, request);
        outcome_matches &= fixture.expected_collision
                               ? result.status ==
                                     RuntimeCollisionQueryStatus::kCollision
                               : result.status ==
                                     RuntimeCollisionQueryStatus::kClear;
        pair_count = result.collision_pairs.size();
        clearance_count = result.clearance_pairs.size();
      } else {
        const CollisionResult result = engine.check(snapshot, query, policy, cancel);
        outcome_matches &=
            result.engine_status == CollisionEngineStatus::kOk &&
            result.in_collision == fixture.expected_collision;
        pair_count = result.normalized_pairs.size();
        clearance_count = result.pair_clearances.size();
        min_clearance = result.min_clearance_m;
        if (fixture.support_contact_policy) {
          auto support = std::find_if(
              result.pair_clearances.begin(), result.pair_clearances.end(),
              [&](const auto& clearance) {
                return clearance.pair == support_pair;
              });
          if (support != result.pair_clearances.end()) {
            support_clearance = support->signed_clearance_m;
          }
        }
      }
      g_count_allocations.store(false, std::memory_order_release);
      const auto cpu_finished = thread_cpu_time_ns();
      const auto wall_finished = std::chrono::steady_clock::now();
      if (!measured) {
        continue;
      }
      wall_ns.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              wall_finished - wall_started)
              .count()));
      cpu_ns.push_back(cpu_finished - cpu_started);
      allocations.push_back(
          g_allocation_count.load(std::memory_order_relaxed));
    }

    if (fixture.support_contact_policy) {
      outcome_matches &= support_clearance.has_value() &&
                         *support_clearance < 0.0 &&
                         *support_clearance >= -0.03 - 1e-9;
    }
    const bool passed = outcome_matches;
    std::ostringstream out;
    out << std::setprecision(17)
        << "{\"record_type\":\"collision_query\""
        << ",\"fixture_id\":\"" << json_escape(fixture.id) << "\""
        << ",\"query_class\":\"" << json_escape(options.query_class)
        << "\""
        << ",\"passed\":" << (passed ? "true" : "false")
        << ",\"sample_count\":" << options.measured_queries
        << ",\"warmup_count\":" << options.warmup_queries
        << ",\"latency_ns\":";
    append_latency(out, wall_ns);
    out << ",\"cpu_time_ns\":";
    append_latency(out, cpu_ns);
    out << ",\"allocation_count\":";
    append_latency(out, allocations);
    out << ",\"engine_id\":\"" << json_escape(engine.engine_id()) << "\""
        << ",\"engine_version\":\"" << json_escape(engine.engine_version())
        << "\""
        << ",\"body_count\":" << snapshot.body_count
        << ",\"geometry_count\":" << snapshot.geometry_count
        << ",\"contains_mesh_geometry\":"
        << (snapshot.contains_mesh_geometry ? "true" : "false")
        << ",\"normalized_pair_count\":" << pair_count
        << ",\"clearance_pair_count\":" << clearance_count;
    if (min_clearance) {
      out << ",\"min_clearance_m\":" << *min_clearance;
    } else {
      out << ",\"min_clearance_m\":null";
    }
    if (support_clearance) {
      out << ",\"support_contact_clearance_m\":" << *support_clearance;
    }
    out << '}';
    std::cout << out.str() << '\n';
    return passed ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "perf_collision_strategy: " << error.what() << '\n';
    return 2;
  }
}
