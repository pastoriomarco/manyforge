#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <roboplan_example_models/resources.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_roboplan/roboplan_planning_context.hpp>

#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using manyforge::core::AddObjectRequestDTO;
using manyforge::core::AttachObjectRequestDTO;
using manyforge::core::BoxShapeDTO;
using manyforge::core::CollisionCancellationToken;
using manyforge::core::CollisionDecisionAction;
using manyforge::core::CollisionDecisionSeverity;
using manyforge::core::CollisionEngineStatus;
using manyforge::core::CollisionPairId;
using manyforge::core::CollisionPolicySnapshot;
using manyforge::core::CollisionQuery;
using manyforge::core::CollisionResult;
using manyforge::core::CollisionShapeDTO;
using manyforge::core::CollisionSurface;
using manyforge::core::CylinderShapeDTO;
using manyforge::core::ICollisionQueryEngine;
using manyforge::core::MeshShapeDTO;
using manyforge::core::ParallelCollisionValidationWorkerPool;
using manyforge::core::Pose3DTO;
using manyforge::core::SupportContactPolicyEntry;
using manyforge::core::TrajectoryDTO;
using manyforge::core::TrajectoryWaypointDTO;
using manyforge::core::Vector3DTO;
using manyforge::collision::ManyForgeCollisionEngine;
using manyforge::roboplan_backend::RoboPlanPlanningContext;
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

AddObjectRequestDTO mesh_object(const std::string& object_id) {
  const auto mesh_path =
      std::filesystem::temp_directory_path() /
      ("manyforge_phase4_collision_" + object_id + ".obj");
  std::ofstream mesh_file(mesh_path);
  mesh_file << "v 0 0 0\n";
  mesh_file << "v 0.01 0 0\n";
  mesh_file << "v 0 0.01 0\n";
  mesh_file << "f 1 2 3\n";
  mesh_file.close();

  MeshShapeDTO mesh;
  mesh.resource_uri = mesh_path.string();
  mesh.scale = Vector3DTO{1.0, 1.0, 1.0};

  AddObjectRequestDTO req;
  req.object_id = object_id;
  req.shape = CollisionShapeDTO{mesh};
  req.pose = pose(0.7, 0.0, 0.25);
  req.frame_id = "universe";
  return req;
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
    const CollisionCancellationToken& cancel,
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
};

void add_error(FixtureEvidence& evidence, std::string error) {
  evidence.setup_ok = false;
  evidence.errors.push_back(std::move(error));
}

bool add_object_checked(RoboPlanPlanningContext& context,
                        const AddObjectRequestDTO& request,
                        FixtureEvidence& evidence) {
  auto result = context.add_object(request);
  if (!result.success) {
    add_error(evidence, "failed to add object " + request.object_id + ": " +
                            (result.error ? result.error->message : "unknown"));
    return false;
  }
  evidence.object_count += 1;
  return true;
}

bool setup_fixture(RoboPlanPlanningContext& context, FixtureEvidence& evidence) {
  const auto& name = evidence.config.name;
  if (name == "primitive_dense_ur5_gripper") {
    for (int i = 0; i < 24; ++i) {
      const Pose3DTO p =
          pose(1.5 + 0.06 * static_cast<double>(i % 8),
               -0.35 + 0.06 * static_cast<double>(i / 8), 0.35);
      if (!add_object_checked(
              context,
              box_object("phase4_box_" + std::to_string(i),
                         Vector3DTO{0.04, 0.04, 0.04}, p),
              evidence)) {
        return false;
      }
    }
    return true;
  }
  if (name == "cylinder_dense_ur5_gripper") {
    for (int i = 0; i < 16; ++i) {
      const Pose3DTO p =
          pose(1.4 + 0.08 * static_cast<double>(i % 8),
               -0.30 + 0.08 * static_cast<double>(i / 8), 0.25);
      if (!add_object_checked(
              context,
              cylinder_object("phase4_cylinder_" + std::to_string(i), 0.025,
                              0.12, p),
              evidence)) {
        return false;
      }
    }
    return true;
  }
  if (name == "attached_object_ur5_gripper") {
    if (!add_object_checked(context,
                            box_object("phase4_attached_box",
                                       Vector3DTO{0.04, 0.04, 0.04},
                                       pose(0.55, 0.0, 0.35)),
                            evidence)) {
      return false;
    }
    AttachObjectRequestDTO attach;
    attach.object_id = "phase4_attached_box";
    attach.frame_id = "tool0";
    auto attach_result = context.attach_object(attach);
    if (!attach_result.success) {
      attach.frame_id = "wrist_3_link";
      attach_result = context.attach_object(attach);
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
  if (name == "mesh_scene_object") {
    return add_object_checked(context, mesh_object("phase4_mesh"), evidence);
  }
  if (name == "isaac_like_primitive_cell") {
    return add_object_checked(context,
                              box_object("phase4_table",
                                         Vector3DTO{0.80, 0.60, 0.05},
                                         pose(1.10, 0.0, 0.10)),
                              evidence) &&
           add_object_checked(context,
                              box_object("phase4_bin_left",
                                         Vector3DTO{0.04, 0.50, 0.16},
                                         pose(1.45, -0.25, 0.24)),
                              evidence) &&
           add_object_checked(context,
                              box_object("phase4_bin_right",
                                         Vector3DTO{0.04, 0.50, 0.16},
                                         pose(1.45, 0.25, 0.24)),
                              evidence) &&
           add_object_checked(context,
                              box_object("phase4_bin_back",
                                         Vector3DTO{0.50, 0.04, 0.16},
                                         pose(1.70, 0.0, 0.24)),
                              evidence) &&
           add_object_checked(context,
                              box_object("phase4_pick_object",
                                         Vector3DTO{0.05, 0.04, 0.03},
                                         pose(0.95, 0.20, 0.18)),
                              evidence);
  }
  if (name == "support_contact_serial_fallback") {
    return add_object_checked(context,
                              box_object("phase4_support_object",
                                         Vector3DTO{0.05, 0.05, 0.05},
                                         pose(1.2, 0.0, 0.20)),
                              evidence) &&
           add_object_checked(context,
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

bool direct_matches_legacy(const CollisionResult& direct,
                           const CollisionResult& legacy) {
  return direct.in_collision == legacy.in_collision &&
         direct.engine_status == legacy.engine_status &&
         direct.decision == legacy.decision;
}

void run_query_probe(RoboPlanPlanningContext& context,
                     const manyforge::core::CollisionWorldSnapshot& snapshot,
                     ManyForgeCollisionEngine& engine,
                     const CollisionPolicySnapshot& policy,
                     FixtureEvidence& evidence) {
  CollisionCancellationToken cancel;
  CollisionQuery query;
  query.query_id = "phase4_" + evidence.config.name;
  query.surface = CollisionSurface::kDiagnosticCompare;
  query.robot_state = context.get_current_state();
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
  const auto legacy = context.collision_check_normalized(query);

  evidence.manyforge_status = direct.engine_status;
  evidence.manyforge_decision = direct.decision;
  evidence.manyforge_in_collision = direct.in_collision;
  evidence.roboplan_status = legacy.engine_status;
  evidence.roboplan_decision = legacy.decision;
  evidence.roboplan_in_collision = legacy.in_collision;
  evidence.manyforge_vs_roboplan_match = direct_matches_legacy(direct, legacy);
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
    RoboPlanPlanningContext& context,
    const manyforge::core::CollisionWorldSnapshot& snapshot,
    const CollisionPolicySnapshot& policy,
    FixtureEvidence& evidence) {
  if (!evidence.config.validate_trajectory) {
    return;
  }
  CollisionCancellationToken cancel;
  const auto trajectory =
      repeated_trajectory(context.get_current_state(),
                          evidence.config.validation_waypoints);

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

FixtureEvidence run_fixture(const FixtureConfig& config) {
  FixtureEvidence evidence;
  evidence.config = config;

  auto bundle = create_native_kernel("ur5_gripper", "arm");
  if (!bundle) {
    add_error(evidence, "failed to create native kernel bundle");
    return evidence;
  }
  auto* context =
      dynamic_cast<RoboPlanPlanningContext*>(bundle->context.get());
  if (context == nullptr) {
    add_error(evidence, "native bundle context is not RoboPlanPlanningContext");
    return evidence;
  }
  if (!setup_fixture(*context, evidence)) {
    return evidence;
  }

  const auto snapshot = context->snapshot();
  if (!snapshot) {
    add_error(evidence, "snapshot publisher returned null");
    return evidence;
  }
  evidence.revision_id = snapshot->revision.revision_id;
  evidence.body_count = snapshot->body_count;
  evidence.geometry_count = snapshot->geometry_count;
  evidence.robot_link_count = snapshot->robot_link_count;
  evidence.detached_object_count = snapshot->detached_object_count;
  evidence.attached_object_count = snapshot->attached_object_count;
  evidence.contains_mesh_geometry = snapshot->contains_mesh_geometry;

  auto engine = make_engine("ur5_gripper", "arm");
  evidence.engine_id = engine->engine_id();
  evidence.engine_version = engine->engine_version();

  const auto policy = policy_for_fixture(config);
  run_query_probe(*context, *snapshot, *engine, policy, evidence);
  if (!config.expect_unsupported_fail_closed) {
    run_validation_probe(*context, *snapshot, policy, evidence);
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
  out << "\"query_us\":{\"p50\":" << e.query_p50_us
      << ",\"p95\":" << e.query_p95_us
      << ",\"p99\":" << e.query_p99_us
      << ",\"max\":" << e.query_max_us << "},";
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
      FixtureConfig{.name = "cylinder_dense_ur5_gripper",
                    .profile = "primitive_cylinder",
                    .expect_manyforge_vs_roboplan_match = true},
      FixtureConfig{.name = "attached_object_ur5_gripper",
                    .profile = "attached_primitive",
                    .expect_manyforge_vs_roboplan_match = true,
                    .expect_attached_object = true},
      FixtureConfig{.name = "mesh_scene_object",
                    .profile = "mesh_scene_object",
                    .expect_manyforge_vs_roboplan_match = true,
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
