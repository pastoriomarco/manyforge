#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <roboplan_example_models/resources.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>

#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_core/execution_collision_support.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::uint64_t> g_allocation_count{0};

[[gnu::noinline]] void* allocate_bytes(std::size_t size) {
  return std::malloc(size == 0 ? 1 : size);
}

[[gnu::noinline]] void* allocate_aligned_bytes(std::size_t size,
                                               std::size_t alignment) {
  void* pointer = nullptr;
  if (posix_memalign(&pointer, alignment, size == 0 ? 1 : size) != 0) {
    return nullptr;
  }
  return pointer;
}

[[gnu::noinline]] void free_bytes(void* pointer) {
  std::free(pointer);
}

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
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  free_bytes(pointer);
}

namespace {

using manyforge::core::AddObjectRequestDTO;
using manyforge::core::AttachObjectRequestDTO;
using manyforge::core::BoxShapeDTO;
using manyforge::core::CollisionCancellationToken;
using manyforge::core::CollisionDecisionAction;
using manyforge::core::CollisionDecisionSeverity;
using manyforge::core::CollisionEngineStatus;
using manyforge::core::CollisionGeometryDescriptor;
using manyforge::core::CollisionGeometryKind;
using manyforge::core::CollisionOwnerKind;
using manyforge::core::CollisionPairId;
using manyforge::core::CollisionPolicySnapshot;
using manyforge::core::CollisionQuery;
using manyforge::core::CollisionResult;
using manyforge::core::CollisionShapeDTO;
using manyforge::core::CollisionSurface;
using manyforge::core::CollisionWorldBody;
using manyforge::core::CollisionWorldSnapshot;
using manyforge::core::CylinderShapeDTO;
using manyforge::core::ICollisionQueryEngine;
using manyforge::core::ParallelCollisionValidationWorkerPool;
using manyforge::core::Pose3DTO;
using manyforge::core::SupportContactPolicyEntry;
using manyforge::core::TrajectoryDTO;
using manyforge::core::TrajectoryWaypointDTO;
using manyforge::core::ValidationOptionsDTO;
using manyforge::core::Vector3DTO;
using manyforge::collision::ManyForgeCollisionEngine;
using manyforge::roboplan_backend::NativeKernelBundle;
using manyforge::roboplan_backend::create_native_kernel;

std::shared_ptr<ManyForgeCollisionEngine> make_engine(
    const std::string& model_name = "ur5_gripper",
    const std::string& group_name = "arm") {
  struct Config {
    std::string urdf_rel;
    std::string srdf_rel;
    std::string default_group;
  };
  const std::map<std::string, Config> configs = {
      {"ur5_gripper",
       {"ur_robot_model/ur5_gripper.urdf", "ur_robot_model/ur5_gripper.srdf",
        "arm"}},
  };
  auto it = configs.find(model_name);
  if (it == configs.end()) {
    throw std::runtime_error("unknown example model: " + model_name);
  }
  const auto models_dir = roboplan::example_models::get_package_models_dir();
  const auto pkg_share = roboplan::example_models::get_package_share_dir();
  const std::string actual_group =
      group_name.empty() ? it->second.default_group : group_name;
  return std::make_shared<ManyForgeCollisionEngine>(
      model_name,
      models_dir / it->second.urdf_rel,
      models_dir / it->second.srdf_rel,
      std::vector<std::filesystem::path>{pkg_share},
      actual_group);
}

manyforge::core::RobotStateDTO observed_state(
    const NativeKernelBundle& bundle) {
  auto state = bundle.observed_state_provider->current_state(
      bundle.clock->now());
  if (!state.ok()) {
    throw std::runtime_error("observed state unavailable in qualification: " +
                             state.error().message);
  }
  return std::move(state.value());
}

std::string json_escape(const std::string& value) {
  std::ostringstream out;
  for (char c : value) {
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
        break;
    }
  }
  return out.str();
}

std::string json_bool(bool value) { return value ? "true" : "false"; }

double percentile_us(std::vector<std::int64_t> values, double percentile) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double rank = percentile * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(rank);
  const auto upper = std::min(lower + 1, values.size() - 1);
  const double fraction = rank - static_cast<double>(lower);
  return (static_cast<double>(values[lower]) * (1.0 - fraction) +
          static_cast<double>(values[upper]) * fraction) /
         1000.0;
}

CollisionPairId canonical_pair(std::string a, std::string b) {
  if (b < a) {
    std::swap(a, b);
  }
  return CollisionPairId{std::move(a), std::move(b)};
}

Pose3DTO pose(double x, double y, double z) {
  Pose3DTO p;
  p.position_m = Vector3DTO{x, y, z};
  p.orientation.w = 1.0;
  return p;
}

AddObjectRequestDTO box_object(std::string id, Vector3DTO dimensions,
                               Pose3DTO object_pose) {
  AddObjectRequestDTO req;
  req.object_id = std::move(id);
  req.shape = CollisionShapeDTO{BoxShapeDTO{dimensions}};
  req.pose = object_pose;
  req.frame_id = "universe";
  return req;
}

AddObjectRequestDTO cylinder_object(std::string id, double radius_m,
                                    double height_m, Pose3DTO object_pose) {
  AddObjectRequestDTO req;
  req.object_id = std::move(id);
  req.shape = CollisionShapeDTO{CylinderShapeDTO{radius_m, height_m}};
  req.pose = object_pose;
  req.frame_id = "universe";
  return req;
}

void append_unloadable_mesh_body(CollisionWorldSnapshot& snapshot) {
  CollisionWorldBody body;
  body.body_id = "object/phase4_unloadable_mesh/shape/0";
  body.owner_kind = CollisionOwnerKind::kDetachedObject;
  body.frame_id = "universe";
  body.pose = pose(0.7, 0.0, 0.25);

  CollisionGeometryDescriptor geom;
  geom.body_id = body.body_id;
  geom.owner_kind = body.owner_kind;
  geom.geometry_kind = CollisionGeometryKind::kTriangleMesh;
  geom.local_pose.orientation.w = 1.0;
  geom.source_hash = "phase4_unloadable_mesh_without_source_uri";
  geom.quality_labels = {"mesh", "collision_mesh", "unloadable_fixture"};
  body.geometries.push_back(std::move(geom));

  snapshot.bodies.push_back(std::move(body));
  snapshot.body_count += 1;
  snapshot.detached_object_count += 1;
  snapshot.geometry_count += 1;
  snapshot.contains_mesh_geometry = true;
}

void append_runtime_collision_probe_body(CollisionWorldSnapshot& snapshot) {
  CollisionWorldBody body;
  body.body_id = "object/runtime_collision_probe/shape/0";
  body.owner_kind = CollisionOwnerKind::kDetachedObject;
  body.frame_id = "universe";
  body.pose = pose(0.0, 0.0, 0.0);

  CollisionGeometryDescriptor geometry;
  geometry.body_id = body.body_id;
  geometry.owner_kind = body.owner_kind;
  geometry.geometry_kind = CollisionGeometryKind::kBox;
  geometry.local_pose.orientation.w = 1.0;
  geometry.box_dimensions_m = Vector3DTO{20.0, 20.0, 20.0};
  body.geometries.push_back(std::move(geometry));

  snapshot.bodies.push_back(std::move(body));
  snapshot.body_count += 1;
  snapshot.detached_object_count += 1;
  snapshot.geometry_count += 1;
}

void extend_runtime_capacity_snapshot(CollisionWorldSnapshot& snapshot,
                                      std::size_t target_body_count) {
  std::size_t object_index = 0;
  while (snapshot.bodies.size() < target_body_count) {
    CollisionWorldBody body;
    body.body_id =
        "object/runtime_capacity_box_" + std::to_string(object_index++);
    body.owner_kind = CollisionOwnerKind::kDetachedObject;
    body.frame_id = "universe";
    body.pose = pose(3.0 + 0.04 * static_cast<double>(object_index % 20),
                     -0.4 +
                         0.04 * static_cast<double>((object_index / 20) % 20),
                     0.2 + 0.04 * static_cast<double>(object_index / 100));

    CollisionGeometryDescriptor geometry;
    geometry.body_id = body.body_id;
    geometry.owner_kind = body.owner_kind;
    geometry.geometry_kind = CollisionGeometryKind::kBox;
    geometry.local_pose.orientation.w = 1.0;
    geometry.box_dimensions_m = Vector3DTO{0.02, 0.02, 0.02};
    body.geometries.push_back(std::move(geometry));
    snapshot.bodies.push_back(std::move(body));
    snapshot.body_count += 1;
    snapshot.detached_object_count += 1;
    snapshot.geometry_count += 1;
  }
  std::sort(snapshot.bodies.begin(), snapshot.bodies.end(),
            [](const CollisionWorldBody& lhs, const CollisionWorldBody& rhs) {
              return lhs.body_id < rhs.body_id;
            });
  snapshot.revision.revision_id += 1;
  snapshot.revision.content_hash = "runtime_capacity_envelope";
}

void make_runtime_capacity_collision(CollisionWorldSnapshot& snapshot) {
  const auto body = std::find_if(
      snapshot.bodies.begin(), snapshot.bodies.end(),
      [](const CollisionWorldBody& candidate) {
        return candidate.owner_kind == CollisionOwnerKind::kDetachedObject;
      });
  if (body == snapshot.bodies.end() || body->geometries.empty()) {
    return;
  }
  body->pose = pose(0.0, 0.0, 0.0);
  body->geometries.front().box_dimensions_m =
      Vector3DTO{20.0, 20.0, 20.0};
}

TrajectoryDTO repeated_trajectory(const manyforge::core::RobotStateDTO& state,
                                  int waypoint_count) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = state.joint_names;
  trajectory.waypoints.reserve(waypoint_count);
  for (int i = 0; i < waypoint_count; ++i) {
    TrajectoryWaypointDTO wp;
    wp.positions_rad = state.positions_rad;
    wp.velocities_rad_s = state.velocities_rad_s;
    wp.time_from_start_s = 0.02 * static_cast<double>(i);
    trajectory.waypoints.push_back(std::move(wp));
  }
  return trajectory;
}

bool same_decision(const manyforge::core::CollisionDecision& a,
                   const manyforge::core::CollisionDecision& b) {
  return a.accepted == b.accepted && a.action == b.action &&
         a.severity == b.severity &&
         a.first_failing_waypoint == b.first_failing_waypoint &&
         a.first_failing_time_ns == b.first_failing_time_ns;
}

bool same_waypoint_records(
    const std::vector<manyforge::core::CollisionValidationWaypointRecord>& a,
    const std::vector<manyforge::core::CollisionValidationWaypointRecord>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].waypoint_index != b[i].waypoint_index ||
        a[i].time_from_start_ns != b[i].time_from_start_ns ||
        a[i].result.engine_status != b[i].result.engine_status ||
        a[i].result.decision != b[i].result.decision ||
        a[i].result.normalized_pairs != b[i].result.normalized_pairs) {
      return false;
    }
  }
  return true;
}

bool same_bundle(
    const manyforge::core::CollisionValidationDecisionBundle& a,
    const manyforge::core::CollisionValidationDecisionBundle& b) {
  return same_decision(a.decision, b.decision) &&
         a.scene_changed_during_validation == b.scene_changed_during_validation &&
         a.cancelled == b.cancelled &&
         a.cancelled_at_waypoint_index == b.cancelled_at_waypoint_index &&
         same_waypoint_records(a.waypoint_records, b.waypoint_records);
}

std::int64_t measure_validation_ns(
    ParallelCollisionValidationWorkerPool& pool,
    const manyforge::core::CollisionWorldSnapshot& snapshot,
    const std::vector<ICollisionQueryEngine*>& engines,
    const TrajectoryDTO& trajectory,
    CollisionCancellationToken& cancel,
    manyforge::core::CollisionValidationDecisionBundle* bundle) {
  const auto start = std::chrono::steady_clock::now();
  *bundle = pool.validate_trajectory(snapshot, engines, trajectory, cancel);
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
      .count();
}

struct FixtureConfig {
  std::string name;
  std::string profile;
  bool expect_manyforge_vs_roboplan_match = true;
  bool expect_unsupported_fail_closed = false;
  bool expect_support_contact_fallback = false;
  bool expect_attached_object = false;
  bool isaac_scene_fixture = false;
  bool query_enabled = true;
  bool runtime_capacity_fixture = false;
  bool validate_trajectory = true;
  int query_iterations = 120;
  int validation_waypoints = 96;
  int validation_runs = 9;
  int worker_count = 4;
};

struct FixtureEvidence {
  FixtureConfig config;
  bool setup_ok = true;
  bool passed = false;
  std::vector<std::string> errors;
  std::string engine_id;
  std::string engine_version;
  std::uint64_t revision_id = 0;
  std::int64_t body_count = 0;
  std::int64_t geometry_count = 0;
  std::int64_t robot_link_count = 0;
  std::int64_t detached_object_count = 0;
  std::int64_t attached_object_count = 0;
  bool contains_mesh_geometry = false;
  int object_count = 0;
  CollisionEngineStatus manyforge_status = CollisionEngineStatus::kEngineError;
  manyforge::core::CollisionDecisionResult manyforge_decision =
      manyforge::core::CollisionDecisionResult::kInconclusiveFail;
  CollisionEngineStatus roboplan_status = CollisionEngineStatus::kEngineError;
  manyforge::core::CollisionDecisionResult roboplan_decision =
      manyforge::core::CollisionDecisionResult::kInconclusiveFail;
  bool manyforge_in_collision = false;
  bool roboplan_in_collision = false;
  bool manyforge_vs_roboplan_match = false;
  bool unsupported_fail_closed = false;
  bool validation_match = true;
  bool support_contact_fallback_verified = false;
  int validation_workers = 0;
  std::string validation_fallback_reason;
  bool validation_support_contact_active = false;
  double query_p50_us = 0.0;
  double query_p95_us = 0.0;
  double query_p99_us = 0.0;
  double query_max_us = 0.0;
  double validation_serial_p95_us = 0.0;
  double validation_parallel_p95_us = 0.0;
  double noncollision_validation_p50_us = 0.0;
  double noncollision_validation_p95_us = 0.0;
  double noncollision_validation_p99_us = 0.0;
  double noncollision_validation_max_us = 0.0;
  std::uint64_t noncollision_validation_allocations = 0;
  bool runtime_query_supported = false;
  std::uint64_t runtime_clean_allocations = 0;
  double runtime_allocations_per_query = 0.0;
  std::uint64_t runtime_allocation_bound_per_query = 0;
  std::uint64_t runtime_wcet_budget_ns = 0;
  std::uint64_t runtime_revision_change_allocations = 0;
  double runtime_revision_change_allocations_per_query = 0.0;
  std::uint64_t runtime_revision_change_allocation_bound_per_query = 0;
  std::uint64_t runtime_revision_change_wcet_budget_ns = 0;
  double runtime_revision_change_p50_us = 0.0;
  double runtime_revision_change_p95_us = 0.0;
  double runtime_revision_change_p99_us = 0.0;
  double runtime_revision_change_max_us = 0.0;
  bool runtime_timeout_fails_closed = false;
  std::uint64_t runtime_timeout_allocations = 0;
  double runtime_timeout_duration_us = 0.0;
  bool runtime_collision_detected = false;
  std::uint64_t runtime_collision_allocations = 0;
  double runtime_collision_p50_us = 0.0;
  double runtime_collision_p95_us = 0.0;
  double runtime_collision_p99_us = 0.0;
  double runtime_collision_max_us = 0.0;
  bool runtime_stale_source_fails_closed = false;
  std::uint64_t runtime_stale_source_allocations = 0;
  double runtime_stale_source_p50_us = 0.0;
  double runtime_stale_source_p95_us = 0.0;
  double runtime_stale_source_p99_us = 0.0;
  double runtime_stale_source_max_us = 0.0;
  double runtime_query_p50_us = 0.0;
  double runtime_query_p95_us = 0.0;
  double runtime_query_p99_us = 0.0;
  double runtime_query_max_us = 0.0;
};

void add_error(FixtureEvidence& evidence, std::string error) {
  evidence.setup_ok = false;
  evidence.errors.push_back(std::move(error));
}

bool add_object_checked(NativeKernelBundle& bundle,
                        const AddObjectRequestDTO& request,
                        FixtureEvidence& evidence) {
  auto result = bundle.scene_authority->add_object(request);
  if (!result.success) {
    add_error(evidence, "failed to add object " + request.object_id + ": " +
                            (result.error ? result.error->message : "unknown"));
    return false;
  }
  evidence.object_count += 1;
  return true;
}

bool setup_fixture(NativeKernelBundle& bundle, FixtureEvidence& evidence) {
  const auto& name = evidence.config.name;
  if (name == "primitive_dense_ur5_gripper") {
    for (int i = 0; i < 24; ++i) {
      const Pose3DTO p =
          pose(1.5 + 0.06 * static_cast<double>(i % 8),
               -0.35 + 0.06 * static_cast<double>(i / 8), 0.35);
      if (!add_object_checked(
              bundle,
              box_object("phase4_box_" + std::to_string(i),
                         Vector3DTO{0.04, 0.04, 0.04}, p),
              evidence)) {
        return false;
      }
    }
    return true;
  }
  if (name == "runtime_capacity_envelope") {
    // The exact-capacity immutable snapshot is constructed in run_fixture()
    // without repeatedly rebuilding the authoritative scene writer.
    return true;
  }
  if (name == "cylinder_dense_ur5_gripper") {
    for (int i = 0; i < 16; ++i) {
      const Pose3DTO p =
          pose(1.4 + 0.08 * static_cast<double>(i % 8),
               -0.30 + 0.08 * static_cast<double>(i / 8), 0.25);
      if (!add_object_checked(
              bundle,
              cylinder_object("phase4_cylinder_" + std::to_string(i), 0.025,
                              0.12, p),
              evidence)) {
        return false;
      }
    }
    return true;
  }
  if (name == "attached_object_ur5_gripper") {
    if (!add_object_checked(bundle,
                            box_object("phase4_attached_box",
                                       Vector3DTO{0.04, 0.04, 0.04},
                                       pose(0.55, 0.0, 0.35)),
                            evidence)) {
      return false;
    }
    AttachObjectRequestDTO attach;
    attach.object_id = "phase4_attached_box";
    attach.frame_id = "tool0";
    auto attach_result = bundle.scene_authority->attach_object(attach);
    if (!attach_result.success) {
      attach.frame_id = "wrist_3_link";
      attach_result = bundle.scene_authority->attach_object(attach);
    }
    if (!attach_result.success) {
      add_error(evidence,
                "failed to attach object: " +
                    (attach_result.error ? attach_result.error->message
                                         : "unknown"));
      return false;
    }
    return true;
  }
  if (name == "unsupported_mesh_fail_closed") {
    return true;
  }
  if (name == "isaac_like_primitive_cell") {
    return add_object_checked(bundle,
                              box_object("phase4_table",
                                         Vector3DTO{0.80, 0.60, 0.05},
                                         pose(1.10, 0.0, 0.10)),
                              evidence) &&
           add_object_checked(bundle,
                              box_object("phase4_bin_left",
                                         Vector3DTO{0.04, 0.50, 0.16},
                                         pose(1.45, -0.25, 0.24)),
                              evidence) &&
           add_object_checked(bundle,
                              box_object("phase4_bin_right",
                                         Vector3DTO{0.04, 0.50, 0.16},
                                         pose(1.45, 0.25, 0.24)),
                              evidence) &&
           add_object_checked(bundle,
                              box_object("phase4_bin_back",
                                         Vector3DTO{0.50, 0.04, 0.16},
                                         pose(1.70, 0.0, 0.24)),
                              evidence) &&
           add_object_checked(bundle,
                              box_object("phase4_pick_object",
                                         Vector3DTO{0.05, 0.04, 0.03},
                                         pose(0.95, 0.20, 0.18)),
                              evidence);
  }
  if (name == "support_contact_serial_fallback") {
    return add_object_checked(bundle,
                              box_object("phase4_support_object",
                                         Vector3DTO{0.05, 0.05, 0.05},
                                         pose(1.2, 0.0, 0.20)),
                              evidence) &&
           add_object_checked(bundle,
                              box_object("phase4_support_fixture",
                                         Vector3DTO{0.30, 0.30, 0.04},
                                         pose(1.2, 0.0, 0.12)),
                              evidence);
  }
  add_error(evidence, "unknown fixture: " + name);
  return false;
}

CollisionPolicySnapshot policy_for_fixture(const FixtureConfig& config) {
  CollisionPolicySnapshot policy;
  policy.policy_id = "phase4_" + config.name;
  policy.revision.revision_id = 1;
  policy.revision.content_hash = "phase4_policy_" + config.name;
  if (config.expect_support_contact_fallback) {
    SupportContactPolicyEntry entry;
    entry.pair = canonical_pair("object/phase4_support_fixture/shape/0",
                                "object/phase4_support_object/shape/0");
    entry.require_separation_by_end = true;
    policy.support_contacts.push_back(std::move(entry));
  }
  return policy;
}

bool direct_matches_diagnostic(const CollisionResult& direct,
                               const CollisionResult& diagnostic) {
  return direct.in_collision == diagnostic.in_collision &&
         direct.engine_status == diagnostic.engine_status &&
         direct.decision == diagnostic.decision;
}

void run_query_probe(NativeKernelBundle& bundle,
                     const manyforge::core::CollisionWorldSnapshot& snapshot,
                     ManyForgeCollisionEngine& engine,
                     const CollisionPolicySnapshot& policy,
                     FixtureEvidence& evidence) {
  CollisionCancellationToken cancel;
  CollisionQuery query;
  query.query_id = "phase4_" + evidence.config.name;
  query.surface = CollisionSurface::kDiagnosticCompare;
  query.robot_state = observed_state(bundle);
  query.collision_world_revision = snapshot.revision;
  query.required_capabilities = {"binary_collision", "body_pair_only"};

  std::vector<std::int64_t> query_ns;
  query_ns.reserve(evidence.config.query_iterations);
  CollisionResult direct;
  for (int i = 0; i < evidence.config.query_iterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    direct = engine.check(snapshot, query, policy, cancel);
    const auto end = std::chrono::steady_clock::now();
    query_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           end - start)
                           .count());
  }
  const auto diagnostic =
      bundle.collision_diagnostic_comparator->compare(query);

  evidence.manyforge_status = direct.engine_status;
  evidence.manyforge_decision = direct.decision;
  evidence.manyforge_in_collision = direct.in_collision;
  evidence.roboplan_status = diagnostic.engine_status;
  evidence.roboplan_decision = diagnostic.decision;
  evidence.roboplan_in_collision = diagnostic.in_collision;
  evidence.manyforge_vs_roboplan_match =
      direct_matches_diagnostic(direct, diagnostic);
  evidence.unsupported_fail_closed =
      direct.engine_status == CollisionEngineStatus::kUnsupportedGeometry &&
      direct.decision ==
          manyforge::core::CollisionDecisionResult::kInconclusiveFail &&
      direct.raw_diagnostics.count("snapshot_sync_error") != 0;

  evidence.query_p50_us = percentile_us(query_ns, 0.50);
  evidence.query_p95_us = percentile_us(query_ns, 0.95);
  evidence.query_p99_us = percentile_us(query_ns, 0.99);
  const auto max_ns = *std::max_element(query_ns.begin(), query_ns.end());
  evidence.query_max_us = static_cast<double>(max_ns) / 1000.0;
}

void run_validation_probe(
    NativeKernelBundle& bundle,
    const manyforge::core::CollisionWorldSnapshot& snapshot,
    const CollisionPolicySnapshot& policy,
    FixtureEvidence& evidence) {
  if (!evidence.config.validate_trajectory) {
    return;
  }
  CollisionCancellationToken cancel;
  const auto trajectory =
      repeated_trajectory(observed_state(bundle),
                          evidence.config.validation_waypoints);

  ValidationOptionsDTO noncollision_options;
  noncollision_options.run_collision_checks = false;
  noncollision_options.max_tcp_linear_speed_mps = 1000.0;
  noncollision_options.tcp_frame = "tool0";
  noncollision_options.base_frame = "base_link";
  for (int i = 0; i < 2; ++i) {
    if (!bundle.trajectory_validator
             ->validate_non_collision(trajectory, noncollision_options, cancel)
             .valid) {
      evidence.errors.push_back(
          "non-collision trajectory validation rejected the probe trajectory");
      return;
    }
  }
  std::vector<std::int64_t> noncollision_validation_ns;
  noncollision_validation_ns.reserve(evidence.config.validation_runs);
  g_allocation_count.store(0, std::memory_order_relaxed);
  g_count_allocations.store(true, std::memory_order_release);
  for (int i = 0; i < evidence.config.validation_runs; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const auto result = bundle.trajectory_validator->validate_non_collision(
        trajectory, noncollision_options, cancel);
    const auto end = std::chrono::steady_clock::now();
    if (!result.valid) {
      g_count_allocations.store(false, std::memory_order_release);
      evidence.errors.push_back(
          "non-collision trajectory validation became nondeterministic");
      return;
    }
    noncollision_validation_ns.push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count());
  }
  g_count_allocations.store(false, std::memory_order_release);
  evidence.noncollision_validation_allocations =
      g_allocation_count.load(std::memory_order_relaxed);
  evidence.noncollision_validation_p50_us =
      percentile_us(noncollision_validation_ns, 0.50);
  evidence.noncollision_validation_p95_us =
      percentile_us(noncollision_validation_ns, 0.95);
  evidence.noncollision_validation_p99_us =
      percentile_us(noncollision_validation_ns, 0.99);
  evidence.noncollision_validation_max_us =
      percentile_us(noncollision_validation_ns, 1.0);

  auto serial_engine = make_engine("ur5_gripper", "arm");
  std::vector<ICollisionQueryEngine*> serial_engines{serial_engine.get()};

  std::vector<std::shared_ptr<ManyForgeCollisionEngine>> parallel_owned;
  std::vector<ICollisionQueryEngine*> parallel_engines;
  parallel_owned.reserve(evidence.config.worker_count);
  parallel_engines.reserve(evidence.config.worker_count);
  for (int i = 0; i < evidence.config.worker_count; ++i) {
    parallel_owned.push_back(make_engine("ur5_gripper", "arm"));
    parallel_engines.push_back(parallel_owned.back().get());
  }

  ParallelCollisionValidationWorkerPool::Config serial_cfg;
  serial_cfg.pass_config.policy_snapshot = policy;
  serial_cfg.worker_count = 1;
  serial_cfg.min_waypoints_for_parallel = 1;
  ParallelCollisionValidationWorkerPool serial_pool(serial_cfg);

  ParallelCollisionValidationWorkerPool::Config parallel_cfg;
  parallel_cfg.pass_config.policy_snapshot = policy;
  parallel_cfg.worker_count = evidence.config.worker_count;
  parallel_cfg.min_waypoints_for_parallel = 1;
  ParallelCollisionValidationWorkerPool parallel_pool(parallel_cfg);

  manyforge::core::CollisionValidationDecisionBundle serial_bundle;
  manyforge::core::CollisionValidationDecisionBundle parallel_bundle;
  (void)measure_validation_ns(serial_pool, snapshot, serial_engines, trajectory,
                              cancel, &serial_bundle);
  (void)measure_validation_ns(parallel_pool, snapshot, parallel_engines,
                              trajectory, cancel, &parallel_bundle);

  std::vector<std::int64_t> serial_validation_ns;
  std::vector<std::int64_t> parallel_validation_ns;
  serial_validation_ns.reserve(evidence.config.validation_runs);
  parallel_validation_ns.reserve(evidence.config.validation_runs);
  for (int i = 0; i < evidence.config.validation_runs; ++i) {
    serial_validation_ns.push_back(measure_validation_ns(
        serial_pool, snapshot, serial_engines, trajectory, cancel,
        &serial_bundle));
    parallel_validation_ns.push_back(measure_validation_ns(
        parallel_pool, snapshot, parallel_engines, trajectory, cancel,
        &parallel_bundle));
  }

  evidence.validation_serial_p95_us =
      percentile_us(serial_validation_ns, 0.95);
  evidence.validation_parallel_p95_us =
      percentile_us(parallel_validation_ns, 0.95);
  evidence.validation_match = same_bundle(serial_bundle, parallel_bundle);
  evidence.validation_workers =
      parallel_pool.last_telemetry().workers_actually_used;
  evidence.validation_fallback_reason =
      parallel_pool.last_telemetry().fallback_reason;
  evidence.validation_support_contact_active =
      parallel_pool.last_telemetry().support_contact_active;
  evidence.support_contact_fallback_verified =
      evidence.validation_support_contact_active &&
      evidence.validation_workers == 1 &&
      evidence.validation_fallback_reason == "support_contact_policy_active";
}

void run_runtime_query_probe(
    NativeKernelBundle& bundle,
    const CollisionWorldSnapshot& snapshot,
    ManyForgeCollisionEngine& engine,
    FixtureEvidence& evidence) {
  const auto state = observed_state(bundle);
  if (engine.prepare_runtime_query(state.joint_names) !=
          manyforge::core::RuntimeCollisionQueryPrepareStatus::kReady ||
      engine.prepare_runtime_snapshot(snapshot) !=
          manyforge::core::RuntimeCollisionQueryPrepareStatus::kReady) {
    evidence.errors.push_back("runtime query preparation failed");
    return;
  }
  manyforge::core::RuntimeCollisionQueryRequest request;
  request.positions_rad = state.positions_rad;
  request.state_time_ns = 1;
  request.timeout_budget_ns = 100'000'000;

  constexpr int kRevisionChangeIterations = 32;
  CollisionWorldSnapshot runtime_snapshot = snapshot;
  std::vector<std::int64_t> revision_change_durations_ns;
  revision_change_durations_ns.reserve(kRevisionChangeIterations);
  g_allocation_count.store(0, std::memory_order_relaxed);
  bool all_revision_changes_conclusive = true;
  for (int i = 0; i < kRevisionChangeIterations; ++i) {
    runtime_snapshot.revision.revision_id += 1;
    const auto started = std::chrono::steady_clock::now();
    g_count_allocations.store(true, std::memory_order_release);
    const auto result = engine.check_runtime(runtime_snapshot, request);
    g_count_allocations.store(false, std::memory_order_release);
    revision_change_durations_ns.push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    all_revision_changes_conclusive =
        all_revision_changes_conclusive &&
        result.status !=
            manyforge::core::RuntimeCollisionQueryStatus::kInconclusive;
  }
  evidence.runtime_revision_change_allocations =
      g_allocation_count.load(std::memory_order_relaxed);
  evidence.runtime_revision_change_allocations_per_query =
      static_cast<double>(evidence.runtime_revision_change_allocations) /
      static_cast<double>(kRevisionChangeIterations);
  evidence.runtime_revision_change_allocation_bound_per_query =
      engine.capability()
          .runtime_query_snapshot_change_allocation_upper_bound;
  evidence.runtime_revision_change_wcet_budget_ns =
      engine.capability().runtime_query_snapshot_change_qualified_wcet_ns;
  evidence.runtime_revision_change_p50_us =
      percentile_us(revision_change_durations_ns, 0.50);
  evidence.runtime_revision_change_p95_us =
      percentile_us(revision_change_durations_ns, 0.95);
  evidence.runtime_revision_change_p99_us =
      percentile_us(revision_change_durations_ns, 0.99);
  evidence.runtime_revision_change_max_us =
      percentile_us(revision_change_durations_ns, 1.0);
  if (!all_revision_changes_conclusive) {
    evidence.errors.push_back(
        "runtime revision-change query became inconclusive under probe");
  }
  if (evidence.runtime_revision_change_allocations >
      evidence.runtime_revision_change_allocation_bound_per_query *
          kRevisionChangeIterations) {
    evidence.errors.push_back(
        "runtime revision-change query exceeded allocation bound");
  }
  if (evidence.runtime_revision_change_max_us * 1000.0 >
      static_cast<double>(evidence.runtime_revision_change_wcet_budget_ns)) {
    evidence.errors.push_back(
        "runtime revision-change query exceeded WCET envelope");
  }

  for (int i = 0; i < 20; ++i) {
    const auto warm = engine.check_runtime(runtime_snapshot, request);
    if (warm.status ==
        manyforge::core::RuntimeCollisionQueryStatus::kInconclusive) {
      evidence.errors.push_back("warm runtime query was inconclusive");
      return;
    }
  }

  constexpr int kIterations = 1000;
  std::vector<std::int64_t> durations_ns;
  durations_ns.reserve(kIterations);
  g_allocation_count.store(0, std::memory_order_relaxed);
  g_count_allocations.store(true, std::memory_order_release);
  bool all_conclusive = true;
  for (int i = 0; i < kIterations; ++i) {
    const auto started = std::chrono::steady_clock::now();
    const auto result = engine.check_runtime(runtime_snapshot, request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    durations_ns.push_back(elapsed);
    all_conclusive =
        all_conclusive &&
        result.status !=
            manyforge::core::RuntimeCollisionQueryStatus::kInconclusive;
  }
  g_count_allocations.store(false, std::memory_order_release);

  auto timeout_request = request;
  timeout_request.timeout_budget_ns = 0;
  g_allocation_count.store(0, std::memory_order_relaxed);
  const auto timeout_started = std::chrono::steady_clock::now();
  g_count_allocations.store(true, std::memory_order_release);
  const auto timeout_result = engine.check_runtime(runtime_snapshot,
                                                   timeout_request);
  g_count_allocations.store(false, std::memory_order_release);
  evidence.runtime_timeout_duration_us =
      static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - timeout_started)
              .count()) /
      1000.0;
  evidence.runtime_timeout_allocations =
      g_allocation_count.load(std::memory_order_relaxed);
  evidence.runtime_timeout_fails_closed =
      timeout_result.status ==
          manyforge::core::RuntimeCollisionQueryStatus::kInconclusive &&
      timeout_result.failure_reason ==
          manyforge::core::RuntimeCollisionQueryFailureReason::kTimeout;
  if (!evidence.runtime_timeout_fails_closed) {
    evidence.errors.push_back("runtime zero-timeout query did not fail closed");
  }
  if (evidence.runtime_timeout_allocations != 0) {
    evidence.errors.push_back("runtime zero-timeout query allocated");
  }

  evidence.runtime_query_supported =
      all_revision_changes_conclusive && all_conclusive;
  evidence.runtime_clean_allocations =
      g_allocation_count.load(std::memory_order_relaxed);
  evidence.runtime_allocations_per_query =
      static_cast<double>(evidence.runtime_clean_allocations) /
      static_cast<double>(kIterations);
  evidence.runtime_allocation_bound_per_query =
      engine.capability().runtime_query_clean_allocation_upper_bound;
  evidence.runtime_wcet_budget_ns =
      engine.capability().runtime_query_qualified_wcet_ns;
  evidence.runtime_query_p50_us = percentile_us(durations_ns, 0.50);
  evidence.runtime_query_p95_us = percentile_us(durations_ns, 0.95);
  evidence.runtime_query_p99_us = percentile_us(durations_ns, 0.99);
  evidence.runtime_query_max_us = percentile_us(durations_ns, 1.0);
  if (!all_conclusive) {
    evidence.errors.push_back("runtime query became inconclusive under probe");
  }
  if (evidence.runtime_clean_allocations >
      evidence.runtime_allocation_bound_per_query * kIterations) {
    evidence.errors.push_back(
        "runtime query exceeded declared warm allocation bound");
  }
  if (evidence.runtime_query_max_us * 1000.0 >
      static_cast<double>(evidence.runtime_wcet_budget_ns)) {
    evidence.errors.push_back("runtime query exceeded declared WCET envelope");
  }

  CollisionWorldSnapshot collision_snapshot = runtime_snapshot;
  collision_snapshot.revision.revision_id += 1;
  if (evidence.config.runtime_capacity_fixture) {
    make_runtime_capacity_collision(collision_snapshot);
  } else {
    append_runtime_collision_probe_body(collision_snapshot);
  }
  const auto collision_prime = engine.check_runtime(collision_snapshot, request);
  constexpr int kCollisionIterations = 64;
  std::vector<std::int64_t> collision_durations_ns;
  collision_durations_ns.reserve(kCollisionIterations);
  bool all_collisions_detected =
      collision_prime.status ==
      manyforge::core::RuntimeCollisionQueryStatus::kCollision;
  g_allocation_count.store(0, std::memory_order_relaxed);
  g_count_allocations.store(true, std::memory_order_release);
  for (int i = 0; i < kCollisionIterations; ++i) {
    const auto started = std::chrono::steady_clock::now();
    const auto result = engine.check_runtime(collision_snapshot, request);
    collision_durations_ns.push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    all_collisions_detected =
        all_collisions_detected &&
        result.status ==
            manyforge::core::RuntimeCollisionQueryStatus::kCollision;
  }
  g_count_allocations.store(false, std::memory_order_release);
  evidence.runtime_collision_detected = all_collisions_detected;
  evidence.runtime_collision_allocations =
      g_allocation_count.load(std::memory_order_relaxed);
  evidence.runtime_collision_p50_us =
      percentile_us(collision_durations_ns, 0.50);
  evidence.runtime_collision_p95_us =
      percentile_us(collision_durations_ns, 0.95);
  evidence.runtime_collision_p99_us =
      percentile_us(collision_durations_ns, 0.99);
  evidence.runtime_collision_max_us =
      percentile_us(collision_durations_ns, 1.0);
  if (!evidence.runtime_collision_detected) {
    evidence.errors.push_back(
        "runtime collision probe did not return collision with pair evidence");
  }
  if (evidence.runtime_collision_allocations >
      evidence.runtime_allocation_bound_per_query * kCollisionIterations) {
    evidence.errors.push_back(
        "runtime collision path exceeded warm allocation bound");
  }
  if (evidence.runtime_collision_max_us * 1000.0 >
      static_cast<double>(evidence.runtime_wcet_budget_ns)) {
    evidence.errors.push_back(
        "runtime collision path exceeded warm WCET envelope");
  }

  CollisionWorldSnapshot stale_world;
  CollisionWorldBody stale_body;
  stale_body.body_id = "object/stale_probe/shape/0";
  stale_body.source_id = "qualification_source";
  stale_body.source_timestamp_ns = 1;
  stale_body.source_freshness_budget_ns = 1;
  stale_world.bodies.push_back(std::move(stale_body));
  const std::vector<manyforge::core::CollisionWorldBodyFreshness>
      no_freshness_sidecar;
  constexpr int kStaleIterations = 1000;
  std::vector<std::int64_t> stale_durations_ns;
  stale_durations_ns.reserve(kStaleIterations);
  bool all_stale = true;
  g_allocation_count.store(0, std::memory_order_relaxed);
  g_count_allocations.store(true, std::memory_order_release);
  for (int i = 0; i < kStaleIterations; ++i) {
    const auto started = std::chrono::steady_clock::now();
    const auto result =
        manyforge::core::check_runtime_dynamic_body_freshness(
            stale_world, no_freshness_sidecar, /*now_ns=*/10,
            /*runtime_cap_ns=*/100, /*lookup_order_prevalidated=*/true);
    stale_durations_ns.push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    all_stale =
        all_stale &&
        result.status ==
            manyforge::core::RuntimeDynamicBodyFreshnessStatus::kStale;
  }
  g_count_allocations.store(false, std::memory_order_release);
  evidence.runtime_stale_source_fails_closed = all_stale;
  evidence.runtime_stale_source_allocations =
      g_allocation_count.load(std::memory_order_relaxed);
  evidence.runtime_stale_source_p50_us =
      percentile_us(stale_durations_ns, 0.50);
  evidence.runtime_stale_source_p95_us =
      percentile_us(stale_durations_ns, 0.95);
  evidence.runtime_stale_source_p99_us =
      percentile_us(stale_durations_ns, 0.99);
  evidence.runtime_stale_source_max_us =
      percentile_us(stale_durations_ns, 1.0);
  if (!evidence.runtime_stale_source_fails_closed) {
    evidence.errors.push_back("runtime stale-source probe did not fail closed");
  }
  if (evidence.runtime_stale_source_allocations != 0) {
    evidence.errors.push_back("runtime stale-source check allocated");
  }
}

FixtureEvidence run_fixture(const FixtureConfig& config) {
  FixtureEvidence evidence;
  evidence.config = config;

  auto bundle = create_native_kernel("ur5_gripper", "arm");
  if (!bundle) {
    add_error(evidence, "failed to create native kernel bundle");
    return evidence;
  }
  if (!setup_fixture(*bundle, evidence)) {
    return evidence;
  }

  const auto snapshot = bundle->scene_authority->snapshot();
  if (!snapshot) {
    add_error(evidence, "snapshot publisher returned null");
    return evidence;
  }
  CollisionWorldSnapshot probe_snapshot = *snapshot;
  if (config.expect_unsupported_fail_closed) {
    append_unloadable_mesh_body(probe_snapshot);
  }
  if (config.runtime_capacity_fixture) {
    extend_runtime_capacity_snapshot(probe_snapshot, 160);
  }
  evidence.revision_id = probe_snapshot.revision.revision_id;
  evidence.body_count = probe_snapshot.body_count;
  evidence.geometry_count = probe_snapshot.geometry_count;
  evidence.robot_link_count = probe_snapshot.robot_link_count;
  evidence.detached_object_count = probe_snapshot.detached_object_count;
  evidence.attached_object_count = probe_snapshot.attached_object_count;
  evidence.contains_mesh_geometry = probe_snapshot.contains_mesh_geometry;
  if (config.runtime_capacity_fixture) {
    evidence.object_count =
        static_cast<int>(probe_snapshot.detached_object_count);
  }

  auto engine = make_engine("ur5_gripper", "arm");
  evidence.engine_id = engine->engine_id();
  evidence.engine_version = engine->engine_version();
  if (config.name == "runtime_capacity_envelope" &&
      static_cast<std::size_t>(evidence.body_count) !=
          engine->capability().runtime_query_max_world_bodies) {
    evidence.errors.push_back(
        "runtime capacity fixture does not match declared body bound");
  }

  const auto policy = policy_for_fixture(config);
  if (config.query_enabled) {
    run_query_probe(*bundle, probe_snapshot, *engine, policy, evidence);
  }
  if (!config.expect_unsupported_fail_closed) {
    run_runtime_query_probe(*bundle, probe_snapshot, *engine, evidence);
    run_validation_probe(*bundle, probe_snapshot, policy, evidence);
  }

  if (config.expect_manyforge_vs_roboplan_match &&
      !evidence.manyforge_vs_roboplan_match) {
    evidence.errors.push_back("ManyForge/RoboPlan shadow decision mismatch");
  }
  if (config.expect_unsupported_fail_closed &&
      !evidence.unsupported_fail_closed) {
    evidence.errors.push_back("unsupported geometry did not fail closed");
  }
  if (config.expect_support_contact_fallback &&
      !evidence.support_contact_fallback_verified) {
    evidence.errors.push_back("support-contact serial fallback not verified");
  }
  if (config.expect_attached_object && evidence.attached_object_count <= 0) {
    evidence.errors.push_back("attached object missing from snapshot");
  }
  if (config.validate_trajectory && !evidence.validation_match) {
    evidence.errors.push_back("serial/parallel validation bundles diverged");
  }
  evidence.passed = evidence.setup_ok && evidence.errors.empty();
  return evidence;
}

std::string fixture_to_json(const FixtureEvidence& e) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(3);
  out << "{";
  out << "\"fixture\":\"" << json_escape(e.config.name) << "\",";
  out << "\"profile\":\"" << json_escape(e.config.profile) << "\",";
  out << "\"passed\":" << json_bool(e.passed) << ",";
  out << "\"engine_id\":\"" << json_escape(e.engine_id) << "\",";
  out << "\"engine_version\":\"" << json_escape(e.engine_version) << "\",";
  out << "\"revision_id\":" << e.revision_id << ",";
  out << "\"body_count\":" << e.body_count << ",";
  out << "\"geometry_count\":" << e.geometry_count << ",";
  out << "\"robot_link_count\":" << e.robot_link_count << ",";
  out << "\"detached_object_count\":" << e.detached_object_count << ",";
  out << "\"attached_object_count\":" << e.attached_object_count << ",";
  out << "\"contains_mesh_geometry\":" << json_bool(e.contains_mesh_geometry)
      << ",";
  out << "\"object_count\":" << e.object_count << ",";
  out << "\"manyforge_collision_status\":"
      << static_cast<int>(e.manyforge_status) << ",";
  out << "\"manyforge_collision_decision\":"
      << static_cast<int>(e.manyforge_decision) << ",";
  out << "\"roboplan_shadow_status\":" << static_cast<int>(e.roboplan_status)
      << ",";
  out << "\"roboplan_shadow_decision\":"
      << static_cast<int>(e.roboplan_decision) << ",";
  out << "\"manyforge_in_collision\":"
      << json_bool(e.manyforge_in_collision) << ",";
  out << "\"roboplan_in_collision\":" << json_bool(e.roboplan_in_collision)
      << ",";
  out << "\"decision_match\":"
      << json_bool(e.manyforge_vs_roboplan_match) << ",";
  out << "\"unsupported_fail_closed\":"
      << json_bool(e.unsupported_fail_closed) << ",";
  out << "\"support_contact_fallback_verified\":"
      << json_bool(e.support_contact_fallback_verified) << ",";
  out << "\"isaac_scene_fixture\":"
      << json_bool(e.config.isaac_scene_fixture) << ",";
  out << "\"query_enabled\":" << json_bool(e.config.query_enabled) << ",";
  out << "\"query_us\":{\"p50\":" << e.query_p50_us
      << ",\"p95\":" << e.query_p95_us
      << ",\"p99\":" << e.query_p99_us
      << ",\"max\":" << e.query_max_us << "},";
  out << "\"runtime_query\":{\"supported\":"
      << json_bool(e.runtime_query_supported)
      << ",\"warm_allocations\":" << e.runtime_clean_allocations
      << ",\"allocations_per_query\":"
      << e.runtime_allocations_per_query
      << ",\"allocation_bound_per_query\":"
      << e.runtime_allocation_bound_per_query
      << ",\"wcet_budget_ns\":" << e.runtime_wcet_budget_ns
      << ",\"p50_us\":" << e.runtime_query_p50_us
      << ",\"p95_us\":" << e.runtime_query_p95_us
      << ",\"p99_us\":" << e.runtime_query_p99_us
      << ",\"max_us\":" << e.runtime_query_max_us
      << ",\"revision_change\":{\"allocations\":"
      << e.runtime_revision_change_allocations
      << ",\"allocations_per_query\":"
      << e.runtime_revision_change_allocations_per_query
      << ",\"allocation_bound_per_query\":"
      << e.runtime_revision_change_allocation_bound_per_query
      << ",\"wcet_budget_ns\":"
      << e.runtime_revision_change_wcet_budget_ns
      << ",\"p50_us\":" << e.runtime_revision_change_p50_us
      << ",\"p95_us\":" << e.runtime_revision_change_p95_us
      << ",\"p99_us\":" << e.runtime_revision_change_p99_us
      << ",\"max_us\":" << e.runtime_revision_change_max_us << "}"
      << ",\"collision\":{\"detected\":"
      << json_bool(e.runtime_collision_detected)
      << ",\"allocations\":" << e.runtime_collision_allocations
      << ",\"p50_us\":" << e.runtime_collision_p50_us
      << ",\"p95_us\":" << e.runtime_collision_p95_us
      << ",\"p99_us\":" << e.runtime_collision_p99_us
      << ",\"max_us\":" << e.runtime_collision_max_us << "}"
      << ",\"stale_source\":{\"fails_closed\":"
      << json_bool(e.runtime_stale_source_fails_closed)
      << ",\"allocations\":" << e.runtime_stale_source_allocations
      << ",\"p50_us\":" << e.runtime_stale_source_p50_us
      << ",\"p95_us\":" << e.runtime_stale_source_p95_us
      << ",\"p99_us\":" << e.runtime_stale_source_p99_us
      << ",\"max_us\":" << e.runtime_stale_source_max_us << "}"
      << ",\"timeout\":{\"fails_closed\":"
      << json_bool(e.runtime_timeout_fails_closed)
      << ",\"allocations\":" << e.runtime_timeout_allocations
      << ",\"duration_us\":" << e.runtime_timeout_duration_us << "}},";
  out << "\"validation\":{\"enabled\":"
      << json_bool(e.config.validate_trajectory)
      << ",\"waypoints\":" << e.config.validation_waypoints
      << ",\"runs\":" << e.config.validation_runs
      << ",\"serial_p95_us\":" << e.validation_serial_p95_us
      << ",\"parallel_p95_us\":" << e.validation_parallel_p95_us
      << ",\"workers\":" << e.validation_workers
      << ",\"fallback_reason\":\""
      << json_escape(e.validation_fallback_reason)
      << "\",\"support_contact_active\":"
      << json_bool(e.validation_support_contact_active)
      << ",\"decision_match\":" << json_bool(e.validation_match) << "},";
  out << "\"noncollision_validation\":{\"enabled\":"
      << json_bool(e.config.validate_trajectory)
      << ",\"runs\":" << e.config.validation_runs
      << ",\"p50_us\":" << e.noncollision_validation_p50_us
      << ",\"p95_us\":" << e.noncollision_validation_p95_us
      << ",\"p99_us\":" << e.noncollision_validation_p99_us
      << ",\"max_us\":" << e.noncollision_validation_max_us
      << ",\"allocations\":"
      << e.noncollision_validation_allocations << "},";
  out << "\"errors\":[";
  for (std::size_t i = 0; i < e.errors.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << "\"" << json_escape(e.errors[i]) << "\"";
  }
  out << "]}";
  return out.str();
}

}  // namespace

int main() {
  const std::vector<FixtureConfig> fixtures = {
      FixtureConfig{.name = "primitive_dense_ur5_gripper",
                    .profile = "primitive_box",
                    .expect_manyforge_vs_roboplan_match = true},
      FixtureConfig{.name = "runtime_capacity_envelope",
                    .profile = "runtime_capacity_bound",
                    .expect_manyforge_vs_roboplan_match = false,
                    .query_enabled = false,
                    .runtime_capacity_fixture = true,
                    .validate_trajectory = false,
                    .query_iterations = 10},
      FixtureConfig{.name = "cylinder_dense_ur5_gripper",
                    .profile = "primitive_cylinder",
                    .expect_manyforge_vs_roboplan_match = true},
      FixtureConfig{.name = "attached_object_ur5_gripper",
                    .profile = "attached_primitive",
                    .expect_manyforge_vs_roboplan_match = true,
                    .expect_attached_object = true},
      FixtureConfig{.name = "unsupported_mesh_fail_closed",
                    .profile = "unsupported_mesh",
                    .expect_manyforge_vs_roboplan_match = false,
                    .expect_unsupported_fail_closed = true,
                    .validate_trajectory = false,
                    .query_iterations = 30},
      FixtureConfig{.name = "isaac_like_primitive_cell",
                    .profile = "isaac_like_primitive",
                    .expect_manyforge_vs_roboplan_match = true,
                    .isaac_scene_fixture = true},
      FixtureConfig{.name = "support_contact_serial_fallback",
                    .profile = "support_contact_policy",
                    .expect_manyforge_vs_roboplan_match = true,
                    .expect_support_contact_fallback = true},
  };

  std::vector<FixtureEvidence> results;
  results.reserve(fixtures.size());
  for (const auto& fixture : fixtures) {
    try {
      results.push_back(run_fixture(fixture));
    } catch (const std::exception& e) {
      FixtureEvidence evidence;
      evidence.config = fixture;
      add_error(evidence, e.what());
      results.push_back(std::move(evidence));
    }
  }

  const int passed_count = static_cast<int>(std::count_if(
      results.begin(), results.end(),
      [](const FixtureEvidence& e) { return e.passed; }));
  const bool all_passed = passed_count == static_cast<int>(results.size());

  std::ostringstream out;
  out << std::fixed << std::setprecision(3);
  out << "{";
  out << "\"phase4_schema\":1,";
  out << "\"qualification_scope\":\"collision_engine_fixture_matrix\",";
  out << "\"fixture_count\":" << results.size() << ",";
  out << "\"fixtures_passed\":" << passed_count << ",";
  out << "\"all_passed\":" << json_bool(all_passed) << ",";
  out << "\"fixtures\":[";
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << fixture_to_json(results[i]);
  }
  out << "]}";
  std::cout << out.str() << "\n";
  return all_passed ? 0 : 3;
}
