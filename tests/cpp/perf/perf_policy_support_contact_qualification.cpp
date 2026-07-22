#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <roboplan_example_models/resources.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sched.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <thread>
#include <utility>
#include <vector>

#ifndef MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
#define MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE 0
#endif

namespace {

using manyforge::collision::ManyForgeCollisionEngine;
using manyforge::core::CollisionCancellationToken;
using manyforge::core::CollisionContactPolicyMode;
using manyforge::core::CollisionDecisionAction;
using manyforge::core::CollisionDecisionSeverity;
using manyforge::core::CollisionEngineStatus;
using manyforge::core::CollisionPairId;
using manyforge::core::CollisionPolicySnapshot;
using manyforge::core::CollisionQuery;
using manyforge::core::CollisionSamplingMode;
using manyforge::core::CollisionSurface;
using manyforge::core::CollisionGeometryDescriptor;
using manyforge::core::CollisionGeometryKind;
using manyforge::core::CollisionOwnerKind;
using manyforge::core::CollisionWorldBody;
using manyforge::core::CollisionWorldSnapshot;
using manyforge::core::CollisionWorldSnapshotBuilder;
using manyforge::core::FkRequestDTO;
using manyforge::core::ICollisionQueryEngine;
using manyforge::core::ParallelCollisionValidationWorkerPool;
using manyforge::core::Pose3DTO;
using manyforge::core::RobotStateDTO;
using manyforge::core::SupportContactPolicyEntry;
using manyforge::core::TrajectoryDTO;
using manyforge::core::TrajectoryWaypointDTO;
using manyforge::core::Vector3DTO;
using manyforge::roboplan_backend::NativeKernelBundle;
using manyforge::roboplan_backend::create_native_kernel;

#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
using manyforge::core::CollisionDistanceRequestMode;
using manyforge::core::ContactReleaseEvidenceMode;
#endif

constexpr std::string_view kPolicyId = "support/step18_policy_perf";
constexpr std::string_view kSupportBodyId =
    "object/support_fixture/shape/0";
constexpr std::string_view kSupportedBodyId =
    "robot/ur5_gripper/attached/supported_object/shape/0";
constexpr double kPositivePenetrationCapM = 0.03;
constexpr double kReleaseToleranceM = 0.001;
constexpr int kDistractorCount = 10;

struct Options {
  bool precise_reference_only = false;
  std::string arm =
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
      "signed_penetration_selective";
#else
      "current_all_active_distance";
#endif
  int waypoint_count = 96;
  int worker_count = 1;
  int warmup_runs = 5;
  int measured_runs = 50;
  std::string run_id = "unspecified";
  std::string source_commit = "unknown";
  std::string specs_commit = "unknown";
  std::string specs_candidate_sha256 = "unknown";
  std::string image_id = "unknown";
  std::string working_tree_candidate_sha256 = "unknown";
  std::string binary_sha256 = "unknown";
  std::string requested_cpu_affinity = "unbound";
  std::string corpus_sha256 = "unknown";
  std::string cpu_governor_set = "unknown";
  std::string cpu_frequency_driver_set = "unknown";
  std::string host_kernel_id = "unknown";
  std::string compiler_flags = "unknown";
  std::string source_role = "current_candidate";
  std::string clean_checkout_manifest_sha256;
  std::string qualification_patch_sha256;
};

std::string environment_or(const char* key, std::string fallback) {
  if (const char* value = std::getenv(key); value != nullptr && *value != '\0') {
    return value;
  }
  return fallback;
}

int parse_positive_int(const std::string& value, const char* option,
                       int maximum = 1'000'000) {
  std::size_t consumed = 0;
  const auto parsed = std::stoll(value, &consumed, 10);
  if (consumed != value.size() || parsed <= 0 || parsed > maximum) {
    throw std::invalid_argument(std::string(option) +
                                " must be a positive bounded integer");
  }
  return static_cast<int>(parsed);
}

bool valid_arm(const std::string& arm) {
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
  return arm == "signed_penetration_selective" ||
         arm == "binary_contact" || arm == "bounded_penetration" ||
         arm == "collision_only";
#else
  return arm == "current_all_active_distance";
#endif
}

bool lowercase_sha256(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

Options parse_options(int argc, char** argv) {
  Options options;
  options.run_id = environment_or("MANYFORGE_QUAL_RUN_ID", options.run_id);
  options.source_commit =
      environment_or("MANYFORGE_SOURCE_COMMIT", options.source_commit);
  options.specs_commit =
      environment_or("MANYFORGE_SPECS_COMMIT", options.specs_commit);
  options.specs_candidate_sha256 = environment_or(
      "MANYFORGE_SPECS_CANDIDATE_SHA256", options.specs_candidate_sha256);
  options.image_id =
      environment_or("MANYFORGE_QUAL_IMAGE_ID", options.image_id);
  options.working_tree_candidate_sha256 = environment_or(
      "MANYFORGE_WORKING_TREE_CANDIDATE_SHA256",
      options.working_tree_candidate_sha256);
  options.binary_sha256 =
      environment_or("MANYFORGE_QUAL_BINARY_SHA256", options.binary_sha256);
  options.requested_cpu_affinity = environment_or(
      "MANYFORGE_STEP18_NONREGRESSION_CPUSET",
      options.requested_cpu_affinity);
  options.corpus_sha256 = environment_or(
      "MANYFORGE_POLICY_SUPPORT_CORPUS_SHA256", options.corpus_sha256);
  options.cpu_governor_set = environment_or(
      "MANYFORGE_CPU_GOVERNOR_SET", options.cpu_governor_set);
  options.cpu_frequency_driver_set = environment_or(
      "MANYFORGE_CPU_FREQUENCY_DRIVER_SET",
      options.cpu_frequency_driver_set);
  options.host_kernel_id =
      environment_or("MANYFORGE_HOST_KERNEL_ID", options.host_kernel_id);
  options.compiler_flags = environment_or(
      "MANYFORGE_MATCHED_COMPILER_FLAGS", options.compiler_flags);
  options.source_role =
      environment_or("MANYFORGE_SOURCE_ROLE", options.source_role);
  options.clean_checkout_manifest_sha256 = environment_or(
      "MANYFORGE_CLEAN_CHECKOUT_MANIFEST_SHA256", "");
  options.qualification_patch_sha256 = environment_or(
      "MANYFORGE_QUALIFICATION_PATCH_SHA256", "");

  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    auto require_value = [&]() -> std::string {
      if (index + 1 >= argc) {
        throw std::invalid_argument(option + " requires a value");
      }
      return argv[++index];
    };
    if (option == "--arm") {
      options.arm = require_value();
    } else if (option == "--precise-reference-only") {
      options.precise_reference_only = true;
    } else if (option == "--waypoints") {
      options.waypoint_count =
          parse_positive_int(require_value(), "--waypoints", 100'000);
    } else if (option == "--workers") {
      options.worker_count =
          parse_positive_int(require_value(), "--workers", 64);
    } else if (option == "--warmup-runs") {
      options.warmup_runs =
          parse_positive_int(require_value(), "--warmup-runs");
    } else if (option == "--measured-runs") {
      options.measured_runs =
          parse_positive_int(require_value(), "--measured-runs");
    } else if (option == "--help" || option == "-h") {
      std::cout
          << "usage: perf_policy_support_contact_qualification --arm ARM "
             "--waypoints N --workers N --warmup-runs N --measured-runs N\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + option);
    }
  }
  if (!options.precise_reference_only && !valid_arm(options.arm)) {
    throw std::invalid_argument("unsupported --arm for this source role: " +
                                options.arm);
  }
  if (options.worker_count != 1 && options.worker_count != 4) {
    throw std::invalid_argument("--workers must be 1 or 4");
  }
  if (!lowercase_sha256(options.corpus_sha256)) {
    throw std::invalid_argument(
        "MANYFORGE_POLICY_SUPPORT_CORPUS_SHA256 must be an exact SHA-256");
  }
  if (options.cpu_governor_set == "unknown" ||
      options.cpu_frequency_driver_set == "unknown" ||
      options.host_kernel_id == "unknown" ||
      options.compiler_flags == "unknown") {
    throw std::invalid_argument(
        "matched scheduler/toolchain provenance environment is required");
  }
  return options;
}

std::shared_ptr<ManyForgeCollisionEngine> make_engine() {
  const auto models_dir = roboplan::example_models::get_package_models_dir();
  const auto package_share = roboplan::example_models::get_package_share_dir();
  return std::make_shared<ManyForgeCollisionEngine>(
      "ur5_gripper", models_dir / "ur_robot_model/ur5_gripper.urdf",
      models_dir / "ur_robot_model/ur5_gripper.srdf",
      std::vector<std::filesystem::path>{package_share}, "arm");
}

struct ProcessEnvironmentObservation {
  std::string effective_cpu_affinity;
  std::string scheduler_policy;
  int scheduler_priority = 0;
  int process_nice = 0;
  std::string kernel_id;
};

std::string affinity_text(const cpu_set_t& set) {
  std::ostringstream out;
  bool wrote = false;
  int begin = -1;
  int previous = -1;
  auto flush = [&] {
    if (begin < 0) {
      return;
    }
    if (wrote) {
      out << ',';
    }
    out << begin;
    if (previous != begin) {
      out << '-' << previous;
    }
    wrote = true;
    begin = -1;
    previous = -1;
  };
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &set)) {
      flush();
      continue;
    }
    if (begin < 0) {
      begin = previous = cpu;
    } else if (cpu == previous + 1) {
      previous = cpu;
    } else {
      flush();
      begin = previous = cpu;
    }
  }
  flush();
  return out.str();
}

const char* scheduler_name(int policy) {
  switch (policy) {
    case SCHED_OTHER:
      return "SCHED_OTHER";
    case SCHED_FIFO:
      return "SCHED_FIFO";
    case SCHED_RR:
      return "SCHED_RR";
#ifdef SCHED_BATCH
    case SCHED_BATCH:
      return "SCHED_BATCH";
#endif
#ifdef SCHED_IDLE
    case SCHED_IDLE:
      return "SCHED_IDLE";
#endif
#ifdef SCHED_DEADLINE
    case SCHED_DEADLINE:
      return "SCHED_DEADLINE";
#endif
    default:
      return "SCHED_UNKNOWN";
  }
}

ProcessEnvironmentObservation observe_process_environment() {
  ProcessEnvironmentObservation observed;
  cpu_set_t set;
  CPU_ZERO(&set);
  if (::sched_getaffinity(0, sizeof(set), &set) != 0) {
    throw std::runtime_error("sched_getaffinity failed");
  }
  observed.effective_cpu_affinity = affinity_text(set);
  const int policy = ::sched_getscheduler(0);
  if (policy < 0) {
    throw std::runtime_error("sched_getscheduler failed");
  }
  observed.scheduler_policy = scheduler_name(policy);
  sched_param parameter{};
  if (::sched_getparam(0, &parameter) != 0) {
    throw std::runtime_error("sched_getparam failed");
  }
  observed.scheduler_priority = parameter.sched_priority;
  errno = 0;
  observed.process_nice = ::getpriority(PRIO_PROCESS, 0);
  if (errno != 0) {
    throw std::runtime_error("getpriority failed");
  }
  utsname name{};
  if (::uname(&name) != 0) {
    throw std::runtime_error("uname failed");
  }
  observed.kernel_id = std::string(name.sysname) + " " + name.release +
                       " " + name.version + " " + name.machine;
  return observed;
}

struct MechanismTotals {
  std::uint64_t query_count = 0;
  std::uint64_t collision_api_call_count = 0;
  std::uint64_t active_collision_pair_count = 0;
  std::uint64_t colliding_geometry_pair_count = 0;
  std::uint64_t colliding_logical_pair_count = 0;
  std::uint64_t distance_api_call_count = 0;
  std::uint64_t distance_evaluated_geometry_pair_count = 0;
  std::uint64_t distance_evaluated_logical_pair_count = 0;
  std::uint64_t fk_placement_invocation_count = 0;
  bool exact_scope_matches = true;
};

class FullTrajectoryObservationEngine final : public ICollisionQueryEngine {
 public:
  FullTrajectoryObservationEngine(
      std::shared_ptr<ManyForgeCollisionEngine> engine,
      std::string expected_arm, std::size_t expected_metric_pair_count)
      : engine_(std::move(engine)),
        expected_arm_(std::move(expected_arm)),
        expected_metric_pair_count_(expected_metric_pair_count) {}

  const manyforge::core::CollisionEngineCapabilityDescriptor& capability()
      const override {
    return engine_->capability();
  }

  manyforge::core::CollisionResult check(
      const CollisionWorldSnapshot& snapshot, const CollisionQuery& query,
      const CollisionPolicySnapshot& policy,
      const CollisionCancellationToken& cancel) override {
    auto result = engine_->check(snapshot, query, policy, cancel);
    ++totals_.query_count;
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
    const auto counters = engine_->internal_last_query_mechanism_counters();
    totals_.collision_api_call_count += counters.collision_api_call_count;
    totals_.active_collision_pair_count +=
        counters.active_collision_pair_count;
    totals_.colliding_geometry_pair_count +=
        counters.colliding_geometry_pair_count;
    totals_.colliding_logical_pair_count +=
        counters.colliding_logical_pair_count;
    totals_.distance_api_call_count += counters.distance_api_call_count;
    totals_.distance_evaluated_geometry_pair_count +=
        counters.distance_evaluated_geometry_pair_count;
    totals_.distance_evaluated_logical_pair_count +=
        counters.distance_evaluated_logical_pair_count;
    totals_.fk_placement_invocation_count +=
        counters.fk_placement_invocation_count;
    const auto expected_mode =
        (expected_arm_ == "signed_penetration_selective" ||
         expected_arm_ == "bounded_penetration")
            ? CollisionDistanceRequestMode::kSelectedCollidingPairs
            : CollisionDistanceRequestMode::kNone;
    totals_.exact_scope_matches =
        totals_.exact_scope_matches &&
        counters.distance_request_mode == expected_mode &&
        counters.collision_api_call_count == 1 &&
        counters.collision_enumeration_completion_ordinal == 1 &&
        counters.fk_placement_invocation_count == 1 &&
        counters.active_collision_pair_count >
            counters.colliding_geometry_pair_count &&
        counters.colliding_geometry_pair_count > 0 &&
        counters.colliding_logical_pair_count > 0;
    if (expected_mode ==
        CollisionDistanceRequestMode::kSelectedCollidingPairs) {
      totals_.exact_scope_matches =
          totals_.exact_scope_matches &&
          counters.first_distance_evaluation_ordinal == 2 &&
          counters.distance_api_call_count ==
              counters.distance_evaluated_geometry_pair_count &&
          counters.distance_evaluated_geometry_pair_count <=
              counters.colliding_geometry_pair_count &&
          counters.distance_evaluated_logical_pair_count ==
              expected_metric_pair_count_;
    } else {
      totals_.exact_scope_matches =
          totals_.exact_scope_matches &&
          counters.first_distance_evaluation_ordinal == 0 &&
          counters.distance_api_call_count == 0 &&
          counters.distance_evaluated_geometry_pair_count == 0 &&
          counters.distance_evaluated_logical_pair_count == 0;
    }
#else
    ++totals_.collision_api_call_count;
    totals_.active_collision_pair_count += result.pair_clearances.size();
    // Every body in this qualification fixture owns exactly one collision
    // geometry, so pristine logical result rows derive exact geometry totals.
    totals_.colliding_geometry_pair_count += result.normalized_pairs.size();
    totals_.colliding_logical_pair_count += result.normalized_pairs.size();
    totals_.distance_evaluated_geometry_pair_count +=
        result.pair_clearances.size();
    totals_.distance_evaluated_logical_pair_count +=
        result.pair_clearances.size();
    ++totals_.distance_api_call_count;
    // Pristine 2bfe30e uses the q-overload for both computeCollisions and the
    // subsequent all-active computeDistances call: two placement/FK updates
    // per waypoint query (manyforge_collision_engine.cpp:1221,1231 there).
    totals_.fk_placement_invocation_count += 2;
    totals_.exact_scope_matches =
        totals_.exact_scope_matches &&
        result.pair_clearances.size() > result.normalized_pairs.size() &&
        !result.normalized_pairs.empty();
#endif
    return result;
  }

  std::string engine_id() const override { return engine_->engine_id(); }
  std::string engine_version() const override {
    return engine_->engine_version();
  }

  const MechanismTotals& totals() const noexcept { return totals_; }

 private:
  std::shared_ptr<ManyForgeCollisionEngine> engine_;
  std::string expected_arm_;
  std::size_t expected_metric_pair_count_ = 0;
  MechanismTotals totals_;
};

RobotStateDTO observed_state(const NativeKernelBundle& bundle) {
  auto state =
      bundle.observed_state_provider->current_state(bundle.clock->now());
  if (!state.ok()) {
    throw std::runtime_error("observed robot state is unavailable: " +
                             state.error().message);
  }
  return std::move(state.value());
}

CollisionWorldBody box_body(std::string id, const Pose3DTO& pose,
                            const Vector3DTO& dimensions) {
  CollisionWorldBody body;
  body.body_id = "object/" + std::move(id) + "/shape/0";
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

CollisionWorldBody attached_box_body(const Pose3DTO& local_pose,
                                     const Vector3DTO& dimensions) {
  CollisionWorldBody body;
  body.body_id = std::string(kSupportedBodyId);
  body.owner_kind = CollisionOwnerKind::kAttachedObject;
  body.parent_link = "robot/ur5_gripper/link/tool0";
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

Pose3DTO compose_pose(const Pose3DTO& parent, const Pose3DTO& child) {
  const double qw = parent.orientation.w;
  const double qx = parent.orientation.x;
  const double qy = parent.orientation.y;
  const double qz = parent.orientation.z;
  const double vx = child.position_m.x_m;
  const double vy = child.position_m.y_m;
  const double vz = child.position_m.z_m;
  const double tx = 2.0 * (qy * vz - qz * vy);
  const double ty = 2.0 * (qz * vx - qx * vz);
  const double tz = 2.0 * (qx * vy - qy * vx);
  const double rotated_x = vx + qw * tx + (qy * tz - qz * ty);
  const double rotated_y = vy + qw * ty + (qz * tx - qx * tz);
  const double rotated_z = vz + qw * tz + (qx * ty - qy * tx);
  Pose3DTO result;
  result.position_m = {
      parent.position_m.x_m + rotated_x,
      parent.position_m.y_m + rotated_y,
      parent.position_m.z_m + rotated_z};
  result.orientation.w =
      qw * child.orientation.w - qx * child.orientation.x -
      qy * child.orientation.y - qz * child.orientation.z;
  result.orientation.x =
      qw * child.orientation.x + qx * child.orientation.w +
      qy * child.orientation.z - qz * child.orientation.y;
  result.orientation.y =
      qw * child.orientation.y - qx * child.orientation.z +
      qy * child.orientation.w + qz * child.orientation.x;
  result.orientation.z =
      qw * child.orientation.z + qx * child.orientation.y -
      qy * child.orientation.x + qz * child.orientation.w;
  return result;
}

CollisionWorldSnapshot build_established_snapshot(
    const CollisionWorldSnapshot& initial, const Pose3DTO& tcp_world_pose) {
  CollisionWorldSnapshotBuilder builder;
  builder.set_publisher_id("rp5_deterministic_corpus")
      .set_revision_id(1)
      .set_emitted_at_ns(1);
  for (const auto& frame : initial.robot_model_frames) {
    builder.add_robot_model_frame(frame);
  }
  for (const auto& body : initial.bodies) {
    builder.add_body(body);
  }
  Pose3DTO attached_local;
  attached_local.orientation.w = 1.0;
  builder.add_body(
      attached_box_body(attached_local, Vector3DTO{0.05, 0.05, 0.05}));
  Pose3DTO support_offset;
  support_offset.position_m.z_m = 0.025;
  support_offset.orientation.w = 1.0;
  const Pose3DTO support_pose = compose_pose(tcp_world_pose, support_offset);
  builder.add_body(box_body("support_fixture", support_pose,
                            Vector3DTO{0.30, 0.30, 0.04}));
  for (int index = 0; index < kDistractorCount; ++index) {
    Pose3DTO pose;
    pose.position_m.x_m = 4.0 + static_cast<double>(index) * 0.75;
    pose.position_m.y_m = 3.0 + static_cast<double>(index % 2);
    pose.position_m.z_m = 2.0 + static_cast<double>(index % 3) * 0.25;
    pose.orientation.w = 1.0;
    builder.add_body(box_body("step18_policy_far_" + std::to_string(index),
                              pose, Vector3DTO{0.03, 0.03, 0.03}));
  }
  auto built = builder.build();
  if (!built.ok()) {
    throw std::runtime_error("failed to build established support fixture: " +
                             built.error().message);
  }
  return std::move(built.value());
}

TrajectoryDTO repeated_trajectory(const RobotStateDTO& state,
                                  int waypoint_count) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = state.joint_names;
  trajectory.waypoints.reserve(static_cast<std::size_t>(waypoint_count));
  for (int index = 0; index < waypoint_count; ++index) {
    TrajectoryWaypointDTO waypoint;
    waypoint.positions_rad = state.positions_rad;
    waypoint.time_from_start_s = static_cast<double>(index) * 0.01;
    trajectory.waypoints.push_back(std::move(waypoint));
  }
  return trajectory;
}

CollisionPairId canonicalize(CollisionPairId pair) {
  if (pair.body_b < pair.body_a) {
    std::swap(pair.body_a, pair.body_b);
  }
  return pair;
}

bool established_support_pair(const CollisionPairId& pair) {
  const auto canonical = canonicalize(pair);
  return canonical == canonicalize(
                          CollisionPairId{std::string(kSupportBodyId),
                                          std::string(kSupportedBodyId)});
}

struct Discovery {
  std::vector<CollisionPairId> physical_collision_pairs;
  std::vector<CollisionPairId> metric_support_pairs;
  double maximum_support_penetration_m = 0.0;
};

Discovery discover_contacts(
    const manyforge::core::CollisionWorldSnapshot& snapshot,
    const RobotStateDTO& state) {
  auto engine = make_engine();
  CollisionQuery query;
  query.query_id = "step18-policy-support-discovery";
  query.surface = CollisionSurface::kDiagnosticCompare;
  query.collision_world_revision = snapshot.revision;
  query.robot_state = state;
  query.waypoint_index = 0;
  query.sampling_mode = CollisionSamplingMode::kWaypointOnly;
  query.required_capabilities = {"body_pair_only", "distance",
                                 "penetration_depth"};
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
  query.distance_request_mode = CollisionDistanceRequestMode::kAllCollidingPairs;
#endif
  CollisionPolicySnapshot free_policy;
  free_policy.policy_id = "step18_discovery_free_space";
  free_policy.revision.revision_id = 1;
  free_policy.revision.content_hash = "step18_discovery_free_space_v1";
  CollisionCancellationToken cancel;
  const auto result = engine->check(snapshot, query, free_policy, cancel);
  if (result.engine_status != CollisionEngineStatus::kOk ||
      !result.in_collision || result.normalized_pairs.empty()) {
    throw std::runtime_error("contact discovery did not collide successfully");
  }

  Discovery discovery;
  bool established_pair_found = false;
  for (const auto& raw_pair : result.normalized_pairs) {
    const auto pair = canonicalize(raw_pair);
    established_pair_found = established_pair_found ||
                             established_support_pair(pair);
    discovery.physical_collision_pairs.push_back(pair);
    const auto clearance = std::find_if(
        result.pair_clearances.begin(), result.pair_clearances.end(),
        [&](const auto& candidate) {
          return canonicalize(candidate.pair) == pair;
        });
    if (clearance == result.pair_clearances.end() ||
        !std::isfinite(clearance->signed_clearance_m) ||
        clearance->signed_clearance_m > 0.0) {
      throw std::runtime_error(
          "contact discovery is missing finite penetration evidence");
    }
    if (established_support_pair(pair)) {
      discovery.metric_support_pairs.push_back(pair);
      discovery.maximum_support_penetration_m =
          std::max(discovery.maximum_support_penetration_m,
                   -clearance->signed_clearance_m);
    }
  }
  auto canonical_unique = [](std::vector<CollisionPairId>& pairs) {
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
  };
  canonical_unique(discovery.physical_collision_pairs);
  canonical_unique(discovery.metric_support_pairs);
  if (!established_pair_found ||
      discovery.physical_collision_pairs.empty() ||
      discovery.metric_support_pairs.size() != 1 ||
      !(discovery.maximum_support_penetration_m > 0.0) ||
      discovery.maximum_support_penetration_m >= kPositivePenetrationCapM) {
    std::ostringstream details;
    details << "discovered contact is outside fixture envelope"
            << " established=" << established_pair_found
            << " physical_pairs="
            << discovery.physical_collision_pairs.size()
            << " metric_pairs=" << discovery.metric_support_pairs.size()
            << " support_penetration_m="
            << discovery.maximum_support_penetration_m;
    throw std::runtime_error(details.str());
  }
  return discovery;
}

class Fingerprint final {
 public:
  void mix(std::string_view value) noexcept {
    for (const unsigned char byte : value) {
      value_ ^= byte;
      value_ *= 1099511628211ULL;
    }
    value_ ^= 0xffU;
    value_ *= 1099511628211ULL;
  }

  void mix(std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
      value_ ^= (value >> shift) & 0xffU;
      value_ *= 1099511628211ULL;
    }
  }

  void mix(double value) noexcept { mix(std::bit_cast<std::uint64_t>(value)); }

  std::string finish() const {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value_;
    return out.str();
  }

 private:
  std::uint64_t value_ = 1469598103934665603ULL;
};

std::string pair_fingerprint(const std::vector<CollisionPairId>& pairs) {
  Fingerprint fingerprint;
  for (const auto& pair : pairs) {
    fingerprint.mix(pair.body_a);
    fingerprint.mix(pair.body_b);
  }
  return fingerprint.finish();
}

std::string state_fingerprint(const RobotStateDTO& state) {
  Fingerprint fingerprint;
  for (const auto& name : state.joint_names) {
    fingerprint.mix(name);
  }
  for (const auto position : state.positions_rad) {
    fingerprint.mix(position);
  }
  return fingerprint.finish();
}

std::string snapshot_fingerprint(
    const manyforge::core::CollisionWorldSnapshot& snapshot) {
  Fingerprint fingerprint;
  fingerprint.mix(static_cast<std::uint64_t>(snapshot.body_count));
  fingerprint.mix(static_cast<std::uint64_t>(snapshot.geometry_count));
  for (const auto& body : snapshot.bodies) {
    fingerprint.mix(body.body_id);
    fingerprint.mix(body.body_content_hash);
  }
  return fingerprint.finish();
}

std::string trajectory_fingerprint(const TrajectoryDTO& trajectory) {
  Fingerprint fingerprint;
  for (const auto& name : trajectory.joint_names) {
    fingerprint.mix(name);
  }
  for (const auto& waypoint : trajectory.waypoints) {
    fingerprint.mix(waypoint.time_from_start_s);
    for (const auto position : waypoint.positions_rad) {
      fingerprint.mix(position);
    }
  }
  return fingerprint.finish();
}

constexpr std::string_view kPreciseReferenceSchema =
    "manyforge.step18.precise_reference_corpus.v1";
constexpr std::string_view kPreciseConfiguredBodyA =
    "object/step18_reference_part/shape/0";
constexpr std::string_view kPreciseConfiguredBodyB =
    "robot/step18_reference_tool/collision/0";
constexpr std::string_view kPreciseUnconfiguredBodyA =
    "fixture/step18_reference_table/shape/0";
constexpr std::string_view kPreciseUnconfiguredBodyB =
    "robot/step18_reference_elbow/collision/0";
constexpr std::string_view kPreciseFarBodyA =
    "fixture/step18_reference_far/shape/0";
constexpr std::string_view kPreciseFarBodyB =
    "robot/step18_reference_tool/collision/0";

CollisionPairId precise_configured_pair() {
  return canonicalize(CollisionPairId{std::string(kPreciseConfiguredBodyA),
                                      std::string(kPreciseConfiguredBodyB)});
}

CollisionPairId precise_unconfigured_pair() {
  return canonicalize(CollisionPairId{std::string(kPreciseUnconfiguredBodyA),
                                      std::string(kPreciseUnconfiguredBodyB)});
}

[[maybe_unused]] CollisionPairId precise_far_pair() {
  return canonicalize(CollisionPairId{std::string(kPreciseFarBodyA),
                                      std::string(kPreciseFarBodyB)});
}

struct PreciseReferenceSample {
  std::vector<CollisionPairId> colliding_pairs;
  std::optional<double> configured_signed_clearance_m;
};

struct PreciseReferenceCase {
  std::string case_id;
  std::vector<PreciseReferenceSample> samples;
  bool enforce_max_start_penetration = true;
  double max_start_penetration_m = 0.005;
  double release_tolerance_m = 1e-5;
  double min_release_progress_m = 0.0;
  bool require_separation_by_end = true;
  bool require_strictly_decreasing_penetration = true;
  bool allow_recontact = false;
};

PreciseReferenceSample precise_sample(
    bool configured_contact, std::optional<double> configured_clearance,
    bool unconfigured_contact = false) {
  PreciseReferenceSample sample;
  if (configured_contact) {
    sample.colliding_pairs.push_back(precise_configured_pair());
  }
  if (unconfigured_contact) {
    sample.colliding_pairs.push_back(precise_unconfigured_pair());
  }
  std::sort(sample.colliding_pairs.begin(), sample.colliding_pairs.end());
  sample.configured_signed_clearance_m = configured_clearance;
  return sample;
}

std::vector<PreciseReferenceCase> precise_reference_cases() {
  using Sample = PreciseReferenceSample;
  std::vector<PreciseReferenceCase> cases;
  auto add = [&](std::string case_id, std::vector<Sample> samples,
                 bool require_separation, bool require_decrease,
                 double max_start = 0.005, double min_progress = 0.0,
                 bool allow_recontact = false) {
    PreciseReferenceCase fixture;
    fixture.case_id = std::move(case_id);
    fixture.samples = std::move(samples);
    fixture.max_start_penetration_m = max_start;
    fixture.min_release_progress_m = min_progress;
    fixture.require_separation_by_end = require_separation;
    fixture.require_strictly_decreasing_penetration = require_decrease;
    fixture.allow_recontact = allow_recontact;
    cases.push_back(std::move(fixture));
  };

  add("decreasing_then_separated_pass",
      {precise_sample(true, -0.004), precise_sample(true, -0.003),
       precise_sample(true, -0.001), precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt)},
      true, true);
  add("absent_configured_pair_no_fabricated_metric_pass",
      {precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt)},
      true, true);
  add("initial_penetration_exceeds_policy",
      {precise_sample(true, -0.006), precise_sample(true, -0.003),
       precise_sample(true, -0.001), precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt)},
      true, true);
  add("penetration_increased",
      {precise_sample(true, -0.004), precise_sample(true, -0.003),
       precise_sample(true, -0.006), precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt)},
      true, true, 0.010);
  add("insufficient_release_progress",
      {precise_sample(true, -0.004), precise_sample(true, -0.004),
       precise_sample(true, -0.004), precise_sample(true, -0.004),
       precise_sample(true, -0.004)},
      false, true, 0.005, 0.001);
  add("colliding_configured_metric_missing",
      {precise_sample(true, -0.003), precise_sample(true, std::nullopt),
       precise_sample(true, -0.001), precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt)},
      true, true);
  add("new_configured_contact_after_clear_start",
      {precise_sample(false, std::nullopt), precise_sample(true, -0.002),
       precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt)},
      false, false);
  add("equal_count_unconfigured_pair_substitution",
      {precise_sample(true, -0.003),
       precise_sample(false, std::nullopt, true),
       precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt)},
      true, true);
  add("recontact_after_release",
      {precise_sample(true, -0.003), precise_sample(false, std::nullopt),
       precise_sample(true, -0.001), precise_sample(false, std::nullopt),
       precise_sample(false, std::nullopt)},
      false, false);
  add("final_sample_not_separated",
      {precise_sample(true, -0.004), precise_sample(true, -0.003),
       precise_sample(true, -0.002), precise_sample(true, -0.001),
       precise_sample(true, -0.0005)},
      true, false);
  return cases;
}

std::string precise_case_input_fingerprint(
    const PreciseReferenceCase& fixture) {
  Fingerprint fingerprint;
  fingerprint.mix(fixture.case_id);
  fingerprint.mix(static_cast<std::uint64_t>(
      fixture.enforce_max_start_penetration));
  fingerprint.mix(fixture.max_start_penetration_m);
  fingerprint.mix(fixture.release_tolerance_m);
  fingerprint.mix(fixture.min_release_progress_m);
  fingerprint.mix(
      static_cast<std::uint64_t>(fixture.require_separation_by_end));
  fingerprint.mix(static_cast<std::uint64_t>(
      fixture.require_strictly_decreasing_penetration));
  fingerprint.mix(static_cast<std::uint64_t>(fixture.allow_recontact));
  fingerprint.mix(fixture.samples.size());
  for (std::size_t index = 0; index < fixture.samples.size(); ++index) {
    fingerprint.mix(index);
    const auto& sample = fixture.samples[index];
    fingerprint.mix(sample.colliding_pairs.size());
    for (const auto& pair : sample.colliding_pairs) {
      fingerprint.mix(pair.body_a);
      fingerprint.mix(pair.body_b);
    }
    fingerprint.mix(static_cast<std::uint64_t>(
        sample.configured_signed_clearance_m.has_value()));
    if (sample.configured_signed_clearance_m) {
      fingerprint.mix(*sample.configured_signed_clearance_m);
    }
  }
  return fingerprint.finish();
}

class PreciseReferenceEngine final : public ICollisionQueryEngine {
 public:
  explicit PreciseReferenceEngine(
      const PreciseReferenceCase& fixture,
      std::chrono::microseconds check_delay =
          std::chrono::microseconds::zero(),
      bool throw_on_check = false)
      : fixture_(fixture),
        check_delay_(check_delay),
        throw_on_check_(throw_on_check) {
    static std::atomic<std::uint64_t> next_context_id{1};
    capability_.engine_id = "step18_precise_reference_engine";
    capability_.engine_version = "1";
    capability_.library_lineage = "deterministic_qualification_fixture";
    capability_.mutable_context_id =
        "step18_precise_reference_context_" +
        std::to_string(next_context_id.fetch_add(1));
    capability_.supported_geometry_kinds = {"box"};
    capability_.supports_distance_query = true;
    capability_.supports_penetration_depth = true;
    capability_.thread_safety = manyforge::core::
        CollisionEngineThreadSafetyClass::kSingleContextExclusive;
    capability_.deterministic_ordering = true;
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
    capability_.supported_distance_request_modes = {
        CollisionDistanceRequestMode::kNone,
        CollisionDistanceRequestMode::kSelectedCollidingPairs,
        CollisionDistanceRequestMode::kAllCollidingPairs,
        CollisionDistanceRequestMode::kAllActivePairs};
#endif
  }

  const manyforge::core::CollisionEngineCapabilityDescriptor& capability()
      const override {
    return capability_;
  }

  manyforge::core::CollisionResult check(
      const CollisionWorldSnapshot& snapshot, const CollisionQuery& query,
      const CollisionPolicySnapshot&,
      const CollisionCancellationToken& cancel) override {
    if (check_delay_ > std::chrono::microseconds::zero()) {
      std::this_thread::sleep_for(check_delay_);
    }
    if (throw_on_check_) {
      throw std::runtime_error("step18 precise reference engine failure");
    }
    manyforge::core::CollisionResult result;
    result.query_id = query.query_id;
    result.surface = query.surface;
    result.state_time_ns = query.state_time_ns;
    result.sampling_mode = query.sampling_mode;
    result.collision_world_revision = snapshot.revision;
    result.engine_id = capability_.engine_id;
    result.engine_version = capability_.engine_version;
    if (cancel.is_cancel_requested()) {
      result.engine_status = CollisionEngineStatus::kCancelled;
      result.decision = manyforge::core::CollisionDecisionResult::
          kInconclusiveFail;
      result.raw_diagnostics["cancel_reason"] = cancel.reason();
      return result;
    }
    if (query.robot_state.positions_rad.empty()) {
      throw std::runtime_error(
          "precise reference query omitted scripted sample index");
    }
    const auto sample_index = static_cast<std::size_t>(
        std::llround(query.robot_state.positions_rad.front()));
    if (sample_index >= fixture_.samples.size()) {
      throw std::runtime_error(
          "precise reference query sample index is out of range");
    }
    const auto& sample = fixture_.samples[sample_index];
    result.normalized_pairs = sample.colliding_pairs;
    result.in_collision = !result.normalized_pairs.empty();
    result.engine_status = CollisionEngineStatus::kOk;
    result.decision = result.in_collision
                          ? manyforge::core::CollisionDecisionResult::kFail
                          : manyforge::core::CollisionDecisionResult::kPass;

    const auto configured = precise_configured_pair();
    const auto configured_colliding =
        std::find(sample.colliding_pairs.begin(), sample.colliding_pairs.end(),
                  configured) != sample.colliding_pairs.end();
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
    if (configured_colliding && sample.configured_signed_clearance_m) {
      result.pair_clearances.push_back(
          {configured, *sample.configured_signed_clearance_m});
    }
#else
    if (sample.configured_signed_clearance_m) {
      result.pair_clearances.push_back(
          {configured, *sample.configured_signed_clearance_m});
    } else if (!configured_colliding) {
      // Pristine 2bfe30e measured every active pair, including absent pairs.
      result.pair_clearances.push_back({configured, 0.010});
    }
    if (std::find(sample.colliding_pairs.begin(), sample.colliding_pairs.end(),
                  precise_unconfigured_pair()) !=
        sample.colliding_pairs.end()) {
      result.pair_clearances.push_back({precise_unconfigured_pair(), -0.002});
    }
    result.pair_clearances.push_back({precise_far_pair(), 1.0});
#endif
    std::sort(result.pair_clearances.begin(), result.pair_clearances.end(),
              [](const auto& left, const auto& right) {
                return left.pair < right.pair;
              });
    for (const auto& clearance : result.pair_clearances) {
      if (!result.min_clearance_m ||
          clearance.signed_clearance_m < *result.min_clearance_m) {
        result.min_clearance_m = clearance.signed_clearance_m;
        result.min_clearance_pair = clearance.pair;
      }
    }
    return result;
  }

  std::string engine_id() const override { return capability_.engine_id; }
  std::string engine_version() const override {
    return capability_.engine_version;
  }

 private:
  const PreciseReferenceCase& fixture_;
  std::chrono::microseconds check_delay_;
  bool throw_on_check_ = false;
  manyforge::core::CollisionEngineCapabilityDescriptor capability_;
};

CollisionPolicySnapshot precise_reference_policy(
    const PreciseReferenceCase& fixture) {
  CollisionPolicySnapshot policy;
  policy.policy_id = "support/step18_precise_reference";
  policy.revision.revision_id = 1;
  policy.revision.content_hash = precise_case_input_fingerprint(fixture);
  policy.require_full_diagnostics = true;
  SupportContactPolicyEntry entry;
  entry.pair = precise_configured_pair();
  entry.mode = CollisionContactPolicyMode::kContactRelease;
  entry.enforce_max_start_penetration =
      fixture.enforce_max_start_penetration;
  entry.max_start_penetration_m = fixture.max_start_penetration_m;
  entry.release_tolerance_m = fixture.release_tolerance_m;
  entry.min_release_progress_m = fixture.min_release_progress_m;
  entry.require_separation_by_end = fixture.require_separation_by_end;
  entry.require_strictly_decreasing_penetration =
      fixture.require_strictly_decreasing_penetration;
  entry.allow_recontact = fixture.allow_recontact;
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
  entry.release_evidence_mode =
      ContactReleaseEvidenceMode::kSignedPenetration;
#endif
  policy.support_contacts.push_back(std::move(entry));
  return policy;
}

TrajectoryDTO precise_reference_trajectory(
    const PreciseReferenceCase& fixture) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"step18_reference_sample_index"};
  for (std::size_t index = 0; index < fixture.samples.size(); ++index) {
    TrajectoryWaypointDTO waypoint;
    waypoint.positions_rad = {static_cast<double>(index)};
    waypoint.time_from_start_s = static_cast<double>(index) * 0.01;
    trajectory.waypoints.push_back(std::move(waypoint));
  }
  return trajectory;
}

struct PreciseReferenceObservation {
  PreciseReferenceCase fixture;
  manyforge::core::CollisionValidationDecisionBundle bundle;
  int workers_effective = 0;
  int workers_used = 0;
  std::string fallback_reason;
};

std::vector<PreciseReferenceObservation> run_precise_reference_corpus(
    const std::shared_ptr<const CollisionWorldSnapshot>& snapshot,
    int worker_count) {
  std::vector<PreciseReferenceObservation> observations;
  for (auto fixture : precise_reference_cases()) {
    ParallelCollisionValidationWorkerPool::Config config;
    config.worker_count = worker_count;
    config.min_waypoints_for_parallel = 1;
    config.pass_config.policy_snapshot = precise_reference_policy(fixture);
    config.pass_config.enforce_contact_release_trajectory_end = true;
    ParallelCollisionValidationWorkerPool pool(config);
    std::vector<std::shared_ptr<ICollisionQueryEngine>> engines;
    for (int index = 0; index < worker_count; ++index) {
      engines.push_back(std::make_shared<PreciseReferenceEngine>(fixture));
    }
    auto cancellation = std::make_shared<CollisionCancellationToken>();
    auto bundle = pool.validate_trajectory(
        snapshot, engines, precise_reference_trajectory(fixture), cancellation,
        config.pass_config);
    const auto telemetry = pool.last_telemetry();
    if (bundle.waypoint_records.size() != fixture.samples.size() ||
        telemetry.workers_effective != worker_count ||
        telemetry.workers_actually_used != worker_count) {
      throw std::runtime_error(
          "precise reference corpus did not use its exact worker profile");
    }
    observations.push_back(PreciseReferenceObservation{
        std::move(fixture), std::move(bundle), telemetry.workers_effective,
        telemetry.workers_actually_used, telemetry.fallback_reason});
  }
  return observations;
}

struct PrecisePartitionProfile {
  int worker_count = 0;
  std::vector<manyforge::core::CollisionValidationDecisionBundle> outcomes;
};

struct PreciseCompletionOrderOutcome {
  std::array<int, 3> order{};
  manyforge::core::CollisionValidationDecisionBundle bundle;
};

struct PreciseExceptionalOutcome {
  std::string case_id;
  std::string fallback_reason;
  manyforge::core::CollisionValidationDecisionBundle bundle;
};

struct PreciseReferenceParityEvidence {
  std::vector<std::vector<PreciseReferenceObservation>> small_case_profiles;
  std::vector<PrecisePartitionProfile> partition_profiles;
  manyforge::core::CollisionValidationDecisionBundle completion_serial;
  std::vector<PreciseCompletionOrderOutcome> completion_orders;
  std::vector<PreciseExceptionalOutcome> exceptional_cases;
};

PreciseReferenceCase exhaustive_partition_fixture(int encoded_pattern) {
  PreciseReferenceCase fixture;
  fixture.case_id = "ordered_contact_partition_pattern_" +
                    std::to_string(encoded_pattern);
  fixture.max_start_penetration_m = 0.10;
  fixture.release_tolerance_m = 1e-5;
  fixture.min_release_progress_m = 0.001;
  fixture.require_separation_by_end = true;
  fixture.require_strictly_decreasing_penetration = true;
  fixture.allow_recontact = false;
  for (int waypoint = 0; waypoint < 5; ++waypoint) {
    switch (encoded_pattern % 4) {
      case 0:
        fixture.samples.push_back(
            precise_sample(false, std::nullopt));
        break;
      case 1:
        fixture.samples.push_back(precise_sample(true, -0.040));
        break;
      case 2:
        fixture.samples.push_back(precise_sample(true, -0.015));
        break;
      default:
        // Pristine all-active distance carried this separated metric; the
        // current selected-colliding representation intentionally omits it.
        fixture.samples.push_back(precise_sample(false, 0.002));
        break;
    }
    encoded_pattern /= 4;
  }
  return fixture;
}

PrecisePartitionProfile run_precise_partition_profile(
    const std::shared_ptr<const CollisionWorldSnapshot>& snapshot,
    int worker_count) {
  constexpr int kPatternCount = 4 * 4 * 4 * 4 * 4;
  auto fixture = exhaustive_partition_fixture(0);
  ParallelCollisionValidationWorkerPool::Config config;
  config.worker_count = worker_count;
  config.min_waypoints_for_parallel = 1;
  config.pass_config.policy_snapshot = precise_reference_policy(fixture);
  config.pass_config.enforce_contact_release_trajectory_end = true;
  ParallelCollisionValidationWorkerPool pool(config);
  std::vector<std::shared_ptr<ICollisionQueryEngine>> engines;
  for (int index = 0; index < worker_count; ++index) {
    engines.push_back(std::make_shared<PreciseReferenceEngine>(fixture));
  }
  PrecisePartitionProfile profile;
  profile.worker_count = worker_count;
  profile.outcomes.reserve(kPatternCount);
  for (int pattern = 0; pattern < kPatternCount; ++pattern) {
    fixture = exhaustive_partition_fixture(pattern);
    auto cancellation = std::make_shared<CollisionCancellationToken>();
    auto bundle = pool.validate_trajectory(
        snapshot, engines, precise_reference_trajectory(fixture), cancellation,
        config.pass_config);
    const auto telemetry = pool.last_telemetry();
    if (bundle.waypoint_records.size() != 5 ||
        telemetry.workers_effective != worker_count ||
        telemetry.workers_actually_used != worker_count) {
      throw std::runtime_error(
          "precise exhaustive partition profile used an unexpected shape");
    }
    profile.outcomes.push_back(std::move(bundle));
  }
  return profile;
}

PreciseReferenceCase completion_order_fixture() {
  PreciseReferenceCase fixture;
  fixture.case_id = "ordered_contact_completion_order";
  fixture.max_start_penetration_m = 0.10;
  fixture.release_tolerance_m = 1e-5;
  fixture.min_release_progress_m = 0.001;
  fixture.require_separation_by_end = true;
  fixture.require_strictly_decreasing_penetration = true;
  fixture.allow_recontact = false;
  for (const double clearance :
       std::array<double, 12>{-0.050, -0.040, -0.030, -0.020,
                              -0.030, -0.020, -0.010, 0.001,
                              0.002, 0.003, 0.004, 0.005}) {
    fixture.samples.push_back(
        clearance < 0.0 ? precise_sample(true, clearance)
                        : precise_sample(false, clearance));
  }
  return fixture;
}

manyforge::core::CollisionValidationDecisionBundle run_precise_fixture(
    const std::shared_ptr<const CollisionWorldSnapshot>& snapshot,
    const PreciseReferenceCase& fixture, int worker_count,
    const std::array<int, 3>* completion_order = nullptr,
    int throwing_worker = -1,
    std::shared_ptr<CollisionCancellationToken> cancellation = nullptr,
    std::string* observed_fallback_reason = nullptr) {
  ParallelCollisionValidationWorkerPool::Config config;
  config.worker_count = worker_count;
  config.min_waypoints_for_parallel = 1;
  config.pass_config.policy_snapshot = precise_reference_policy(fixture);
  config.pass_config.enforce_contact_release_trajectory_end = true;
  ParallelCollisionValidationWorkerPool pool(config);
  std::array<int, 3> delay_rank{0, 0, 0};
  if (completion_order != nullptr) {
    for (std::size_t rank = 0; rank < completion_order->size(); ++rank) {
      delay_rank[static_cast<std::size_t>((*completion_order)[rank])] =
          static_cast<int>(rank);
    }
  }
  std::vector<std::shared_ptr<ICollisionQueryEngine>> engines;
  for (int index = 0; index < worker_count; ++index) {
    const auto delay = completion_order != nullptr
                           ? std::chrono::microseconds(
                                 delay_rank[static_cast<std::size_t>(index)] *
                                 50)
                           : std::chrono::microseconds::zero();
    engines.push_back(std::make_shared<PreciseReferenceEngine>(
        fixture, delay, index == throwing_worker));
  }
  if (!cancellation) {
    cancellation = std::make_shared<CollisionCancellationToken>();
  }
  auto bundle = pool.validate_trajectory(
      snapshot, engines, precise_reference_trajectory(fixture), cancellation,
      config.pass_config);
  if (observed_fallback_reason != nullptr) {
    *observed_fallback_reason = pool.last_telemetry().fallback_reason;
  }
  return bundle;
}

PreciseExceptionalOutcome run_precise_expired_deadline(
    const std::shared_ptr<const CollisionWorldSnapshot>& snapshot) {
  auto fixture = exhaustive_partition_fixture(0);
  manyforge::core::FakeClock clock;
  manyforge::core::ManualScheduler scheduler;
  ParallelCollisionValidationWorkerPool::Config config;
  config.worker_count = 2;
  config.min_waypoints_for_parallel = 1;
  config.validation_wall_timeout = std::chrono::milliseconds(20);
  config.clock = &clock;
  config.scheduler = &scheduler;
  config.pass_config.policy_snapshot = precise_reference_policy(fixture);
  config.pass_config.enforce_contact_release_trajectory_end = true;
  ParallelCollisionValidationWorkerPool pool(config);
  std::vector<std::shared_ptr<ICollisionQueryEngine>> engines;
  engines.push_back(std::make_shared<PreciseReferenceEngine>(fixture));
  engines.push_back(std::make_shared<PreciseReferenceEngine>(fixture));
  auto cancellation = std::make_shared<CollisionCancellationToken>();
  auto bundle = pool.validate_trajectory(
      snapshot, engines, precise_reference_trajectory(fixture), cancellation,
      config.pass_config, std::optional<manyforge::core::TimePoint>{0});
  return PreciseExceptionalOutcome{
      "expired_absolute_deadline", pool.last_telemetry().fallback_reason,
      std::move(bundle)};
}

PreciseReferenceParityEvidence collect_precise_reference_parity(
    const std::shared_ptr<const CollisionWorldSnapshot>& snapshot) {
  PreciseReferenceParityEvidence evidence;
  for (int workers = 1; workers <= 5; ++workers) {
    evidence.small_case_profiles.push_back(
        run_precise_reference_corpus(snapshot, workers));
  }
  for (int workers = 1; workers <= 5; ++workers) {
    evidence.partition_profiles.push_back(
        run_precise_partition_profile(snapshot, workers));
  }

  const auto completion_fixture = completion_order_fixture();
  evidence.completion_serial =
      run_precise_fixture(snapshot, completion_fixture, 1);
  const std::array<std::array<int, 3>, 6> completion_orders{{
      {{0, 1, 2}}, {{0, 2, 1}}, {{1, 0, 2}},
      {{1, 2, 0}}, {{2, 0, 1}}, {{2, 1, 0}},
  }};
  for (const auto& order : completion_orders) {
    evidence.completion_orders.push_back(PreciseCompletionOrderOutcome{
        order, run_precise_fixture(snapshot, completion_fixture, 3, &order)});
  }

  auto exceptional_fixture = exhaustive_partition_fixture(0);
  while (exceptional_fixture.samples.size() < 8) {
    exceptional_fixture.samples.push_back(
        precise_sample(false, std::nullopt));
  }
  exceptional_fixture.case_id = "exceptional_control_flow";
  auto pre_cancel = std::make_shared<CollisionCancellationToken>();
  pre_cancel->request_cancel("operator_stop");
  std::string pre_cancel_fallback;
  auto pre_cancel_bundle = run_precise_fixture(
      snapshot, exceptional_fixture, 2, nullptr, -1,
      std::move(pre_cancel), &pre_cancel_fallback);
  evidence.exceptional_cases.push_back(PreciseExceptionalOutcome{
      "pre_cancelled", std::move(pre_cancel_fallback),
      std::move(pre_cancel_bundle)});
  evidence.exceptional_cases.push_back(run_precise_expired_deadline(snapshot));
  std::string worker_exception_fallback;
  auto worker_exception =
      run_precise_fixture(snapshot, exceptional_fixture, 2, nullptr, 1,
                          nullptr, &worker_exception_fallback);
  evidence.exceptional_cases.push_back(PreciseExceptionalOutcome{
      "worker_exception", std::move(worker_exception_fallback),
      std::move(worker_exception)});
  return evidence;
}

CollisionPolicySnapshot make_policy(
    const std::string& arm,
    const std::vector<CollisionPairId>& metric_support_pairs,
    const std::vector<CollisionPairId>& physical_collision_pairs,
    double bounded_cap_m = kPositivePenetrationCapM) {
  CollisionPolicySnapshot policy;
  policy.policy_id = std::string(kPolicyId);
  policy.revision.revision_id = 1;
  policy.require_full_diagnostics = true;
  if (arm == "collision_only") {
    policy.allowed_touch_link_pairs = physical_collision_pairs;
  } else {
    for (const auto& pair : physical_collision_pairs) {
      if (std::find(metric_support_pairs.begin(), metric_support_pairs.end(),
                    pair) == metric_support_pairs.end()) {
        policy.allowed_touch_link_pairs.push_back(pair);
      }
    }
    for (const auto& pair : metric_support_pairs) {
      SupportContactPolicyEntry entry;
      entry.pair = pair;
      entry.mode = CollisionContactPolicyMode::kContactRelease;
      entry.enforce_max_start_penetration = true;
      entry.max_start_penetration_m = kPositivePenetrationCapM;
      entry.release_tolerance_m = kReleaseToleranceM;
      entry.min_release_progress_m = 0.0;
      entry.require_separation_by_end = false;
      entry.require_strictly_decreasing_penetration = false;
      entry.allow_recontact = true;
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
      if (arm == "binary_contact") {
        entry.release_evidence_mode =
            ContactReleaseEvidenceMode::kBinaryContact;
        entry.enforce_max_start_penetration = false;
        entry.max_start_penetration_m = 0.0;
        entry.release_tolerance_m = 0.0;
      } else {
        entry.release_evidence_mode =
            ContactReleaseEvidenceMode::kSignedPenetration;
      }
      if (arm == "bounded_penetration") {
        entry.mode = CollisionContactPolicyMode::kBoundedPenetration;
        entry.enforce_max_start_penetration = false;
        entry.max_start_penetration_m = 0.0;
        entry.release_tolerance_m = 0.0;
        entry.max_contact_penetration_m = bounded_cap_m;
        entry.allow_recontact = false;
      }
#endif
      policy.support_contacts.push_back(std::move(entry));
    }
  }
  Fingerprint content;
  content.mix(policy.policy_id);
  content.mix(static_cast<std::uint64_t>(policy.require_full_diagnostics));
  for (const auto& pair : policy.allowed_touch_link_pairs) {
    content.mix(pair.body_a);
    content.mix(pair.body_b);
    content.mix("allowed_touch_link");
  }
  for (const auto& entry : policy.support_contacts) {
    content.mix(entry.pair.body_a);
    content.mix(entry.pair.body_b);
    content.mix(static_cast<std::uint64_t>(entry.mode));
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
    if (entry.mode == CollisionContactPolicyMode::kBoundedPenetration) {
      content.mix(entry.max_contact_penetration_m);
      continue;
    }
    content.mix(static_cast<std::uint64_t>(entry.release_evidence_mode));
#else
    (void)bounded_cap_m;
#endif
    content.mix(static_cast<std::uint64_t>(
        entry.enforce_max_start_penetration));
    content.mix(entry.max_start_penetration_m);
    content.mix(entry.release_tolerance_m);
    content.mix(entry.min_release_progress_m);
    content.mix(static_cast<std::uint64_t>(entry.require_separation_by_end));
    content.mix(static_cast<std::uint64_t>(
        entry.require_strictly_decreasing_penetration));
    content.mix(static_cast<std::uint64_t>(entry.allow_recontact));
  }
  policy.revision.content_hash = content.finish();
  return policy;
}

struct OutputSummary {
  bool accepted = false;
  CollisionDecisionAction action = CollisionDecisionAction::kDispatch;
  CollisionDecisionSeverity severity = CollisionDecisionSeverity::kNone;
  std::optional<std::int64_t> first_failing_waypoint;
  std::optional<std::int64_t> first_failing_time_ns;
  bool all_engine_status_ok = true;
  bool every_waypoint_in_collision = true;
  std::size_t waypoint_record_count = 0;
  std::size_t normalized_pair_observation_count = 0;
  std::size_t clearance_observation_count = 0;
  std::size_t last_waypoint_pair_count = 0;
  std::size_t last_waypoint_clearance_count = 0;
  std::size_t finite_colliding_clearance_observation_count = 0;
  double maximum_observed_penetration_m = 0.0;
  std::string normalized_pair_output_fingerprint;
  std::string colliding_clearance_output_fingerprint;
};

OutputSummary summarize(
    const manyforge::core::CollisionValidationDecisionBundle& bundle,
    const std::vector<CollisionPairId>& expected_pairs) {
  OutputSummary summary;
  summary.accepted = bundle.decision.accepted;
  summary.action = bundle.decision.action;
  summary.severity = bundle.decision.severity;
  summary.first_failing_waypoint =
      bundle.decision.first_failing_waypoint;
  summary.first_failing_time_ns = bundle.decision.first_failing_time_ns;
  summary.waypoint_record_count = bundle.waypoint_records.size();
  Fingerprint physical;
  Fingerprint clearances;
  physical.mix(static_cast<std::uint64_t>(bundle.decision.accepted));
  physical.mix(static_cast<std::uint64_t>(bundle.decision.action));
  physical.mix(static_cast<std::uint64_t>(bundle.decision.severity));
  physical.mix(bundle.waypoint_records.size());
  for (const auto& record : bundle.waypoint_records) {
    summary.all_engine_status_ok =
        summary.all_engine_status_ok &&
        record.result.engine_status == CollisionEngineStatus::kOk;
    summary.every_waypoint_in_collision =
        summary.every_waypoint_in_collision && record.result.in_collision;
    auto observed = record.result.normalized_pairs;
    for (auto& pair : observed) {
      pair = canonicalize(std::move(pair));
    }
    std::sort(observed.begin(), observed.end());
    observed.erase(std::unique(observed.begin(), observed.end()),
                   observed.end());
    if (observed != expected_pairs) {
      throw std::runtime_error(
          "trajectory physical collision pairs differ from discovery");
    }
    summary.normalized_pair_observation_count += observed.size();
    summary.clearance_observation_count +=
        record.result.pair_clearances.size();
    summary.last_waypoint_pair_count = observed.size();
    summary.last_waypoint_clearance_count =
        record.result.pair_clearances.size();
    physical.mix(static_cast<std::uint64_t>(record.waypoint_index));
    physical.mix(static_cast<std::uint64_t>(record.result.engine_status));
    physical.mix(static_cast<std::uint64_t>(record.result.in_collision));
    for (const auto& pair : observed) {
      physical.mix(pair.body_a);
      physical.mix(pair.body_b);
      const auto clearance = std::find_if(
          record.result.pair_clearances.begin(),
          record.result.pair_clearances.end(), [&](const auto& candidate) {
            return canonicalize(candidate.pair) == pair;
          });
      if (clearance == record.result.pair_clearances.end() ||
          !std::isfinite(clearance->signed_clearance_m) ||
          clearance->signed_clearance_m > 0.0) {
        continue;
      }
      ++summary.finite_colliding_clearance_observation_count;
      summary.maximum_observed_penetration_m =
          std::max(summary.maximum_observed_penetration_m,
                   -clearance->signed_clearance_m);
      clearances.mix(static_cast<std::uint64_t>(record.waypoint_index));
      clearances.mix(pair.body_a);
      clearances.mix(pair.body_b);
      clearances.mix(clearance->signed_clearance_m);
    }
  }
  summary.normalized_pair_output_fingerprint = physical.finish();
  summary.colliding_clearance_output_fingerprint = clearances.finish();
  return summary;
}

std::int64_t process_cpu_time_ns() noexcept {
  timespec value{};
  if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000LL +
         static_cast<std::int64_t>(value.tv_nsec);
}

double percentile_us(std::vector<std::int64_t> values, double fraction) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double rank = fraction * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(rank);
  const auto upper = std::min(lower + 1, values.size() - 1);
  const double weight = rank - static_cast<double>(lower);
  return (static_cast<double>(values[lower]) * (1.0 - weight) +
          static_cast<double>(values[upper]) * weight) /
         1000.0;
}

void append_latency(std::ostringstream& out,
                    const std::vector<std::int64_t>& values) {
  out << std::fixed << std::setprecision(3)
      << "{\"p50\":" << percentile_us(values, 0.50)
      << ",\"p95\":" << percentile_us(values, 0.95)
      << ",\"p99\":" << percentile_us(values, 0.99)
      << ",\"max\":" << percentile_us(values, 1.0) << "}";
}

std::string json_escape(std::string_view value) {
  std::ostringstream out;
  for (const char character : value) {
    switch (character) {
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
        out << character;
        break;
    }
  }
  return out.str();
}

void append_pair_json(std::ostringstream& out, const CollisionPairId& pair) {
  const auto canonical = canonicalize(pair);
  out << "{\"body_a\":\"" << json_escape(canonical.body_a)
      << "\",\"body_b\":\"" << json_escape(canonical.body_b) << "\"}";
}

template <typename T>
void append_optional_integer_json(std::ostringstream& out,
                                  const std::optional<T>& value) {
  if (value) {
    out << *value;
  } else {
    out << "null";
  }
}

void append_optional_double_json(std::ostringstream& out,
                                 const std::optional<double>& value) {
  if (value) {
    out << std::setprecision(17) << *value;
  } else {
    out << "null";
  }
}

void append_optional_string_json(
    std::ostringstream& out, const std::optional<std::string>& value) {
  if (value) {
    out << '"' << json_escape(*value) << '"';
  } else {
    out << "null";
  }
}

void append_optional_revision_json(
    std::ostringstream& out,
    const std::optional<manyforge::core::CollisionWorldRevision>& revision) {
  if (revision) {
    out << "{\"revision_id\":" << revision->revision_id
        << ",\"content_hash\":\"" << json_escape(revision->content_hash)
        << "\"}";
  } else {
    out << "null";
  }
}

void append_revision_json(
    std::ostringstream& out,
    const manyforge::core::CollisionWorldRevision& revision) {
  out << "{\"revision_id\":" << revision.revision_id
      << ",\"content_hash\":\"" << json_escape(revision.content_hash)
      << "\"}";
}

void append_precise_stable_result_fields(
    std::ostringstream& out,
    const manyforge::core::CollisionResult& result) {
  out << "\"query_id\":";
  append_optional_string_json(out, result.query_id);
  out << ",\"collision_world_revision\":";
  append_optional_revision_json(out, result.collision_world_revision);
  out << ",\"policy_revision\":";
  append_optional_string_json(out, result.policy_revision);
  out << ",\"policy_revision_used\":";
  append_optional_string_json(out, result.policy_revision_used);
  out << ",\"surface_code\":" << static_cast<int>(result.surface)
      << ",\"state_time_ns\":";
  append_optional_integer_json(out, result.state_time_ns);
  out << ",\"engine_id\":\"" << json_escape(result.engine_id)
      << "\",\"engine_version\":\"" << json_escape(result.engine_version)
      << "\",\"sampling_mode_code\":"
      << static_cast<int>(result.sampling_mode)
      << ",\"contact_body_a\":";
  append_optional_string_json(out, result.contact_body_a);
  out << ",\"contact_body_b\":";
  append_optional_string_json(out, result.contact_body_b);
  out << ",\"query_duration_ns_omitted_as_volatile\":true";
}

void append_precise_raw_distance_fields(
    std::ostringstream& out,
    const manyforge::core::CollisionResult& result) {
  out << "\"raw_distance_scope\":\""
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
      << "selected_colliding_pairs"
#else
      << "all_active_pairs"
#endif
      << "\",\"raw_distance_request_mode_field_present\":"
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
      << "true,\"raw_distance_request_mode_code\":"
      << static_cast<int>(result.distance_request_mode)
      << ",\"raw_evaluated_distance_pair_count_field_present\":true"
      << ",\"raw_evaluated_distance_pair_count\":"
      << result.evaluated_distance_pair_count
#else
      << "false,\"raw_distance_request_mode_code\":null,"
         "\"raw_evaluated_distance_pair_count_field_present\":false"
      << ",\"raw_evaluated_distance_pair_count\":"
      << result.pair_clearances.size()
#endif
      << ",\"raw_pair_clearances\":[";
  for (std::size_t index = 0; index < result.pair_clearances.size();
       ++index) {
    if (index != 0) {
      out << ',';
    }
    const auto& clearance = result.pair_clearances[index];
    out << "{\"pair\":";
    append_pair_json(out, clearance.pair);
    out << ",\"signed_clearance_m\":" << std::setprecision(17)
        << clearance.signed_clearance_m << '}';
  }
  out << "],\"raw_min_clearance_m\":";
  append_optional_double_json(out, result.min_clearance_m);
  out << ",\"raw_min_clearance_pair\":";
  if (result.min_clearance_pair) {
    append_pair_json(out, *result.min_clearance_pair);
  } else {
    out << "null";
  }
}

bool precise_volatile_diagnostic(std::string_view key) {
  return key == "snapshot_sync_duration_ns" ||
         key == "total_call_duration_ns";
}

void mix_optional_integer(Fingerprint& fingerprint,
                          const std::optional<std::int64_t>& value) {
  fingerprint.mix(static_cast<std::uint64_t>(value.has_value()));
  if (value) {
    fingerprint.mix(static_cast<std::uint64_t>(*value));
  }
}

std::string precise_structural_transcript_fingerprint(
    const manyforge::core::CollisionValidationDecisionBundle& bundle) {
  Fingerprint fingerprint;
  fingerprint.mix(static_cast<std::uint64_t>(bundle.decision.accepted));
  fingerprint.mix(static_cast<std::uint64_t>(bundle.decision.action));
  fingerprint.mix(static_cast<std::uint64_t>(bundle.decision.severity));
  mix_optional_integer(fingerprint,
                       bundle.decision.first_failing_waypoint);
  mix_optional_integer(fingerprint, bundle.decision.first_failing_time_ns);
  fingerprint.mix(static_cast<std::uint64_t>(bundle.cancelled));
  fingerprint.mix(static_cast<std::uint64_t>(
      bundle.cancellation_reason.has_value()));
  if (bundle.cancellation_reason) {
    fingerprint.mix(*bundle.cancellation_reason);
  }
  mix_optional_integer(fingerprint, bundle.cancelled_at_waypoint_index);
  fingerprint.mix(static_cast<std::uint64_t>(
      bundle.scene_changed_during_validation));
  fingerprint.mix(static_cast<std::uint64_t>(bundle.churn_window_exceeded));
  fingerprint.mix(static_cast<std::uint64_t>(
      bundle.pose_source_retries_used));
  fingerprint.mix(bundle.waypoint_records.size());
  for (const auto& record : bundle.waypoint_records) {
    fingerprint.mix(static_cast<std::uint64_t>(record.waypoint_index));
    mix_optional_integer(fingerprint, record.time_from_start_ns);
    fingerprint.mix(
        static_cast<std::uint64_t>(record.produced_decision_change));
    fingerprint.mix(static_cast<std::uint64_t>(record.result.engine_status));
    fingerprint.mix(static_cast<std::uint64_t>(record.result.decision));
    fingerprint.mix(static_cast<std::uint64_t>(record.result.in_collision));
    auto pairs = record.result.normalized_pairs;
    for (auto& pair : pairs) {
      pair = canonicalize(std::move(pair));
    }
    std::sort(pairs.begin(), pairs.end());
    fingerprint.mix(pairs.size());
    for (const auto& pair : pairs) {
      fingerprint.mix(pair.body_a);
      fingerprint.mix(pair.body_b);
    }
    auto classified = record.result.normalized_pairs_classified;
    std::sort(classified.begin(), classified.end(),
              [](const auto& left, const auto& right) {
                return canonicalize(left.pair) < canonicalize(right.pair);
              });
    fingerprint.mix(classified.size());
    for (const auto& entry : classified) {
      const auto pair = canonicalize(entry.pair);
      fingerprint.mix(pair.body_a);
      fingerprint.mix(pair.body_b);
      fingerprint.mix(static_cast<std::uint64_t>(entry.classification));
      fingerprint.mix(static_cast<std::uint64_t>(entry.is_first_failure));
    }
  }
  return fingerprint.finish();
}

void append_nonvolatile_diagnostics_json(
    std::ostringstream& out,
    const std::map<std::string, std::string>& diagnostics,
    bool omit_deadline_elapsed = false) {
  out << '{';
  bool wrote = false;
  for (const auto& [key, value] : diagnostics) {
    if (precise_volatile_diagnostic(key) ||
        (omit_deadline_elapsed && key == "elapsed_ns")) {
      continue;
    }
    if (wrote) {
      out << ',';
    }
    wrote = true;
    out << '\"' << json_escape(key) << "\":\"" << json_escape(value)
        << '\"';
  }
  out << '}';
}

void append_precise_compact_bundle(
    std::ostringstream& out,
    const manyforge::core::CollisionValidationDecisionBundle& bundle,
    bool omit_deadline_elapsed = false) {
  out << "{\"decision\":{\"accepted\":"
      << (bundle.decision.accepted ? "true" : "false")
      << ",\"action_code\":" << static_cast<int>(bundle.decision.action)
      << ",\"severity_code\":"
      << static_cast<int>(bundle.decision.severity)
      << ",\"first_failing_waypoint\":";
  append_optional_integer_json(out, bundle.decision.first_failing_waypoint);
  out << ",\"first_failing_time_ns\":";
  append_optional_integer_json(out, bundle.decision.first_failing_time_ns);
  out << ",\"collision_world_revision\":";
  append_optional_revision_json(out,
                                bundle.decision.collision_world_revision);
  out << ",\"failure_archive_id\":";
  append_optional_string_json(out, bundle.decision.failure_archive_id);
  out << "},\"cancelled\":" << (bundle.cancelled ? "true" : "false")
      << ",\"cancellation_reason\":";
  if (bundle.cancellation_reason) {
    out << '\"' << json_escape(*bundle.cancellation_reason) << '\"';
  } else {
    out << "null";
  }
  out << ",\"cancelled_at_waypoint_index\":";
  append_optional_integer_json(out, bundle.cancelled_at_waypoint_index);
  out << ",\"borrowed_world_revision\":";
  append_revision_json(out, bundle.borrowed_world_revision);
  out << ",\"final_world_revision\":";
  append_optional_revision_json(out, bundle.final_world_revision);
  out << ",\"scene_changed_during_validation\":"
      << (bundle.scene_changed_during_validation ? "true" : "false")
      << ",\"churn_window_exceeded\":"
      << (bundle.churn_window_exceeded ? "true" : "false")
      << ",\"pose_source_retries_used\":"
      << bundle.pose_source_retries_used
      << ",\"waypoint_record_count\":" << bundle.waypoint_records.size()
      << ",\"structural_transcript_fingerprint_fnv1a64\":\""
      << precise_structural_transcript_fingerprint(bundle)
      << "\",\"waypoint_records\":[";
  for (std::size_t index = 0; index < bundle.waypoint_records.size();
       ++index) {
    if (index != 0) {
      out << ',';
    }
    const auto& record = bundle.waypoint_records[index];
    const auto& result = record.result;
    out << "{\"waypoint_index\":" << record.waypoint_index
        << ",\"time_from_start_ns\":";
    append_optional_integer_json(out, record.time_from_start_ns);
    out << ',';
    append_precise_stable_result_fields(out, result);
    out << ",\"produced_decision_change\":"
        << (record.produced_decision_change ? "true" : "false")
        << ",\"engine_status_code\":"
        << static_cast<int>(result.engine_status)
        << ",\"decision_code\":" << static_cast<int>(result.decision)
        << ",\"in_collision\":"
        << (result.in_collision ? "true" : "false")
        << ",\"normalized_pairs\":[";
    for (std::size_t pair_index = 0;
         pair_index < result.normalized_pairs.size(); ++pair_index) {
      if (pair_index != 0) {
        out << ',';
      }
      append_pair_json(out, result.normalized_pairs[pair_index]);
    }
    out << "],\"classified_pairs\":[";
    for (std::size_t pair_index = 0;
         pair_index < result.normalized_pairs_classified.size();
         ++pair_index) {
      if (pair_index != 0) {
        out << ',';
      }
      const auto& classified =
          result.normalized_pairs_classified[pair_index];
      out << "{\"pair\":";
      append_pair_json(out, classified.pair);
      out << ",\"classification_code\":"
          << static_cast<int>(classified.classification)
          << ",\"is_first_failure\":"
          << (classified.is_first_failure ? "true" : "false") << '}';
    }
    out << "],";
    append_precise_raw_distance_fields(out, result);
    out << ",\"nonvolatile_raw_diagnostics\":";
    append_nonvolatile_diagnostics_json(out,
                                        result.raw_diagnostics,
                                        omit_deadline_elapsed);
    const auto omitted_volatile = static_cast<std::size_t>(std::count_if(
        result.raw_diagnostics.begin(), result.raw_diagnostics.end(),
        [](const auto& entry) {
          return precise_volatile_diagnostic(entry.first);
        }));
    out << ",\"omitted_volatile_diagnostic_count\":"
        << omitted_volatile
        << ",\"omitted_exceptional_volatile_diagnostic_keys\":[";
    if (omit_deadline_elapsed &&
        result.raw_diagnostics.contains("elapsed_ns")) {
      out << "\"elapsed_ns\"";
    }
    out << "]}";
  }
  out << "]}";
}

void append_precise_reference_corpus(
    std::ostringstream& out,
    const std::vector<PreciseReferenceObservation>& observations,
    int requested_workers) {
  out << "{\"schema_version\":\"" << kPreciseReferenceSchema
      << "\",\"execution_surface\":"
         "\"actual_parallel_collision_validation_worker_pool_untimed\""
      << ",\"worker_count_requested\":" << requested_workers
      << ",\"case_count\":" << observations.size()
      << ",\"normalization_contract\":{"
         "\"allowed_semantic_delta\":"
         "\"closed_step18_precise_reference_migration\","
         "\"baseline_distance_scope\":\"all_active_pairs\","
         "\"current_distance_scope\":\"selected_colliding_pairs\","
         "\"clearance_projection\":"
         "\"configured_intersection_colliding_pairs\","
         "\"normalized_release_evidence_mode\":\"signed_penetration\","
         "\"volatile_diagnostic_keys\":["
         "\"snapshot_sync_duration_ns\",\"total_call_duration_ns\"]},"
         "\"case_order\":[";
  for (std::size_t index = 0; index < observations.size(); ++index) {
    if (index != 0) {
      out << ',';
    }
    out << '\"' << json_escape(observations[index].fixture.case_id) << '\"';
  }
  out << "],\"cases\":[";
  for (std::size_t case_index = 0; case_index < observations.size();
       ++case_index) {
    if (case_index != 0) {
      out << ',';
    }
    const auto& observation = observations[case_index];
    const auto& fixture = observation.fixture;
    const auto& bundle = observation.bundle;
    out << "{\"case_id\":\"" << json_escape(fixture.case_id)
        << "\",\"input_fingerprint_fnv1a64\":\""
        << precise_case_input_fingerprint(fixture) << "\",\"input\":{"
        << "\"policy\":{\"mode\":\"contact_release\","
           "\"release_evidence_mode\":\"signed_penetration\","
           "\"raw_release_evidence_mode_field_present\":"
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
        << "true,\"raw_release_evidence_mode\":\"signed_penetration\","
#else
        << "false,\"raw_release_evidence_mode\":null,"
#endif
           "\"enforce_max_start_penetration\":"
        << (fixture.enforce_max_start_penetration ? "true" : "false")
        << ",\"max_start_penetration_m\":" << std::setprecision(17)
        << fixture.max_start_penetration_m
        << ",\"release_tolerance_m\":" << fixture.release_tolerance_m
        << ",\"min_release_progress_m\":"
        << fixture.min_release_progress_m
        << ",\"require_separation_by_end\":"
        << (fixture.require_separation_by_end ? "true" : "false")
        << ",\"require_strictly_decreasing_penetration\":"
        << (fixture.require_strictly_decreasing_penetration ? "true"
                                                             : "false")
        << ",\"allow_recontact\":"
        << (fixture.allow_recontact ? "true" : "false")
        << "},\"configured_pair\":";
    append_pair_json(out, precise_configured_pair());
    out << ",\"samples\":[";
    for (std::size_t sample_index = 0;
         sample_index < fixture.samples.size(); ++sample_index) {
      if (sample_index != 0) {
        out << ',';
      }
      const auto& sample = fixture.samples[sample_index];
      out << "{\"waypoint_index\":" << sample_index
          << ",\"time_from_start_ns\":"
          << static_cast<std::int64_t>(sample_index) * 10'000'000
          << ",\"colliding_pairs\":[";
      for (std::size_t pair_index = 0;
           pair_index < sample.colliding_pairs.size(); ++pair_index) {
        if (pair_index != 0) {
          out << ',';
        }
        append_pair_json(out, sample.colliding_pairs[pair_index]);
      }
      out << "],\"configured_signed_clearance_m\":";
      append_optional_double_json(out,
                                  sample.configured_signed_clearance_m);
      out << '}';
    }
    out << "]},\"execution\":{\"worker_count_effective\":"
        << observation.workers_effective << ",\"workers_used\":"
        << observation.workers_used << ",\"fallback_reason\":\""
        << json_escape(observation.fallback_reason) << "\"},\"output\":{"
        << "\"decision\":{\"accepted\":"
        << (bundle.decision.accepted ? "true" : "false")
        << ",\"action_code\":"
        << static_cast<int>(bundle.decision.action)
        << ",\"severity_code\":"
        << static_cast<int>(bundle.decision.severity)
        << ",\"first_failing_waypoint\":";
    append_optional_integer_json(out,
                                 bundle.decision.first_failing_waypoint);
    out << ",\"first_failing_time_ns\":";
    append_optional_integer_json(out,
                                 bundle.decision.first_failing_time_ns);
    out << ",\"collision_world_revision\":";
    append_optional_revision_json(out,
                                  bundle.decision.collision_world_revision);
    out << ",\"failure_archive_id\":";
    append_optional_string_json(out, bundle.decision.failure_archive_id);
    out << "},\"borrowed_world_revision\":";
    append_revision_json(out, bundle.borrowed_world_revision);
    out << ",\"final_world_revision\":";
    append_optional_revision_json(out, bundle.final_world_revision);
    out << ",\"pose_source_retries_used\":"
        << bundle.pose_source_retries_used
        << ",\"scene_changed_during_validation\":"
        << (bundle.scene_changed_during_validation ? "true" : "false")
        << ",\"churn_window_exceeded\":"
        << (bundle.churn_window_exceeded ? "true" : "false")
        << ",\"cancelled\":" << (bundle.cancelled ? "true" : "false")
        << ",\"cancellation_reason\":";
    if (bundle.cancellation_reason) {
      out << '"' << json_escape(*bundle.cancellation_reason) << '"';
    } else {
      out << "null";
    }
    out << ",\"cancelled_at_waypoint_index\":";
    append_optional_integer_json(out, bundle.cancelled_at_waypoint_index);
    out << ",\"waypoint_records\":[";
    for (std::size_t record_index = 0;
         record_index < bundle.waypoint_records.size(); ++record_index) {
      if (record_index != 0) {
        out << ',';
      }
      const auto& record = bundle.waypoint_records[record_index];
      const auto& result = record.result;
      out << "{\"waypoint_index\":" << record.waypoint_index
          << ",\"time_from_start_ns\":";
      append_optional_integer_json(out, record.time_from_start_ns);
      out << ',';
      append_precise_stable_result_fields(out, result);
      out << ",\"produced_decision_change\":"
          << (record.produced_decision_change ? "true" : "false")
          << ",\"engine_status_code\":"
          << static_cast<int>(result.engine_status)
          << ",\"decision_code\":" << static_cast<int>(result.decision)
          << ",\"in_collision\":"
          << (result.in_collision ? "true" : "false")
          << ",\"normalized_pairs\":[";
      for (std::size_t pair_index = 0;
           pair_index < result.normalized_pairs.size(); ++pair_index) {
        if (pair_index != 0) {
          out << ',';
        }
        append_pair_json(out, result.normalized_pairs[pair_index]);
      }
      out << "],\"classified_pairs\":[";
      for (std::size_t pair_index = 0;
           pair_index < result.normalized_pairs_classified.size();
           ++pair_index) {
        if (pair_index != 0) {
          out << ',';
        }
        const auto& classified =
            result.normalized_pairs_classified[pair_index];
        out << "{\"pair\":";
        append_pair_json(out, classified.pair);
        out << ",\"classification_code\":"
            << static_cast<int>(classified.classification)
            << ",\"is_first_failure\":"
            << (classified.is_first_failure ? "true" : "false") << '}';
      }
      out << "],\"raw_distance_scope\":\""
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
          << "selected_colliding_pairs"
#else
          << "all_active_pairs"
#endif
          << "\",\"raw_evaluated_distance_pair_count\":"
 #if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
          << result.evaluated_distance_pair_count
 #else
          << result.pair_clearances.size()
 #endif
          << ",\"raw_distance_request_mode_field_present\":"
 #if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
          << "true,\"raw_distance_request_mode_code\":"
          << static_cast<int>(result.distance_request_mode)
          << ",\"raw_evaluated_distance_pair_count_field_present\":true"
 #else
          << "false,\"raw_distance_request_mode_code\":null,"
             "\"raw_evaluated_distance_pair_count_field_present\":false"
 #endif
          << ",\"raw_pair_clearances\":[";
      for (std::size_t clearance_index = 0;
           clearance_index < result.pair_clearances.size();
           ++clearance_index) {
        if (clearance_index != 0) {
          out << ',';
        }
        const auto& clearance = result.pair_clearances[clearance_index];
        out << "{\"pair\":";
        append_pair_json(out, clearance.pair);
        out << ",\"signed_clearance_m\":" << std::setprecision(17)
            << clearance.signed_clearance_m << '}';
      }
      out << "],\"raw_min_clearance_m\":";
      append_optional_double_json(out, result.min_clearance_m);
      out << ",\"raw_min_clearance_pair\":";
      if (result.min_clearance_pair) {
        append_pair_json(out, *result.min_clearance_pair);
      } else {
        out << "null";
      }
      out << ",\"nonvolatile_raw_diagnostics\":{";
      bool wrote_diagnostic = false;
      std::size_t omitted_volatile = 0;
      for (const auto& [key, value] : result.raw_diagnostics) {
        if (precise_volatile_diagnostic(key)) {
          ++omitted_volatile;
          continue;
        }
        if (wrote_diagnostic) {
          out << ',';
        }
        wrote_diagnostic = true;
        out << '\"' << json_escape(key) << "\":\""
            << json_escape(value) << '\"';
      }
      out << "},\"omitted_volatile_diagnostic_count\":"
          << omitted_volatile << '}';
    }
    out << "]}}";
  }
  out << "]}";
}

void append_precise_reference_parity_evidence(
    std::ostringstream& out,
    const PreciseReferenceParityEvidence& evidence) {
  out << "{\"normalization_contract\":{"
         "\"id\":\"step18_precise_reference_normalization.v1\","
         "\"allowed_semantic_delta\":"
         "\"closed_step18_precise_reference_migration\","
         "\"clearance_projection\":"
         "\"configured_intersection_colliding_pairs\","
         "\"baseline_distance_scope\":\"all_active_pairs\","
         "\"current_distance_scope\":\"selected_colliding_pairs\","
         "\"normalized_release_evidence_mode\":\"signed_penetration\","
         "\"baseline_zero_only_legacy_diagnostic_count\":1,"
         "\"legacy_baseline_diagnostic\":{\"field\":\"contact_release_min_"
      << "separation_clearance_m\",\"required_baseline_value\":0,"
         "\"required_baseline_serialized_value\":\"0.000000\","
         "\"baseline_presence\":\"required_on_violation_records\","
         "\"current_presence\":\"forbidden\"},"
         "\"raw_release_evidence_mode_field\":{"
         "\"baseline_presence\":false,\"baseline_value\":null,"
         "\"current_presence\":true,"
         "\"current_value\":\"signed_penetration\"},"
         "\"missing_metric_classification_migration\":{"
         "\"case_id\":\"colliding_configured_metric_missing\","
         "\"waypoint_index\":1,\"pair\":{"
         "\"body_a\":\"object/step18_reference_part/shape/0\","
         "\"body_b\":\"robot/step18_reference_tool/collision/0\"},"
         "\"baseline_classification_code\":3,"
         "\"current_classification_code\":8,"
         "\"normalized_classification_code\":8,"
         "\"reason\":\"missing_contact_release_clearance\","
         "\"accepted\":false,\"action_code\":3,"
         "\"severity_code\":2,\"first_failing_waypoint\":1,"
         "\"first_failing_time_ns\":10000000},"
         "\"volatile_result_fields\":[\"query_duration_ns\"],"
         "\"global_volatile_diagnostic_keys\":["
         "\"snapshot_sync_duration_ns\",\"total_call_duration_ns\"],"
         "\"exceptional_volatile_diagnostic_keys\":{"
         "\"expired_absolute_deadline\":[\"elapsed_ns\"]}},"
         "\"orchestration_projection_contract\":{"
         "\"id\":\"step18_cross_worker_orchestration_projection.v1\","
         "\"scope\":\"cross_worker_count_only\","
         "\"excluded_result_fields\":[\"query_id\"],"
         "\"matched_source_profile_parity\":"
         "\"full_transcript_including_query_id\"},"
         "\"small_case_profiles\":[";
  for (std::size_t profile_index = 0;
       profile_index < evidence.small_case_profiles.size(); ++profile_index) {
    if (profile_index != 0) {
      out << ',';
    }
    const auto& profile = evidence.small_case_profiles[profile_index];
    if (profile.empty()) {
      throw std::runtime_error("empty precise small-case profile");
    }
    append_precise_reference_corpus(
        out, profile, static_cast<int>(profile_index + 1));
  }
  out << "],\"partition_inventory\":{"
         "\"sample_kind_count\":4,\"waypoint_count\":5,"
         "\"encoding\":\"little_endian_base4\","
         "\"sample_kinds\":["
         "{\"kind\":\"absent\",\"signed_clearance_m\":null},"
         "{\"kind\":\"colliding\",\"signed_clearance_m\":-0.04},"
         "{\"kind\":\"colliding\",\"signed_clearance_m\":-0.015},"
         "{\"kind\":\"separated\",\"signed_clearance_m\":0.002}],"
         "\"patterns_per_worker\":1024,"
         "\"serial_worker_count\":1,"
         "\"partition_worker_counts\":[2,3,4,5],"
         "\"profiles\":[";
  for (std::size_t profile_index = 0;
       profile_index < evidence.partition_profiles.size(); ++profile_index) {
    if (profile_index != 0) {
      out << ',';
    }
    const auto& profile = evidence.partition_profiles[profile_index];
    out << "{\"worker_count\":" << profile.worker_count
        << ",\"outcome_count\":" << profile.outcomes.size()
        << ",\"outcomes\":[";
    for (std::size_t pattern = 0; pattern < profile.outcomes.size();
         ++pattern) {
      if (pattern != 0) {
        out << ',';
      }
      const auto fixture =
          exhaustive_partition_fixture(static_cast<int>(pattern));
      out << "{\"pattern_index\":" << pattern
          << ",\"sample_kind_indices\":[";
      auto encoded_pattern = pattern;
      for (int waypoint = 0; waypoint < 5; ++waypoint) {
        if (waypoint != 0) {
          out << ',';
        }
        out << (encoded_pattern % 4);
        encoded_pattern /= 4;
      }
      out << "],\"input_fingerprint_fnv1a64\":\""
          << precise_case_input_fingerprint(fixture)
          << "\",\"transcript\":";
      append_precise_compact_bundle(out, profile.outcomes[pattern]);
      out << '}';
    }
    out << "]}";
  }
  out << "]},\"completion_order_inventory\":{"
         "\"worker_count\":3,\"permutation_count\":6,"
         "\"input_signed_clearances_m\":[";
  const auto completion_fixture = completion_order_fixture();
  for (std::size_t index = 0;
       index < completion_fixture.samples.size(); ++index) {
    if (index != 0) {
      out << ',';
    }
    append_optional_double_json(
        out,
        completion_fixture.samples[index].configured_signed_clearance_m);
  }
  out << "],"
         "\"input_fingerprint_fnv1a64\":\""
      << precise_case_input_fingerprint(completion_fixture)
      << "\",\"serial_reference\":";
  append_precise_compact_bundle(out, evidence.completion_serial);
  out << ",\"orders\":[";
  for (std::size_t index = 0; index < evidence.completion_orders.size();
       ++index) {
    if (index != 0) {
      out << ',';
    }
    const auto& outcome = evidence.completion_orders[index];
    out << "{\"order\":[" << outcome.order[0] << ',' << outcome.order[1]
        << ',' << outcome.order[2] << "],\"transcript\":";
    append_precise_compact_bundle(out, outcome.bundle);
    out << '}';
  }
  out << "]},\"exceptional_cases\":[";
  for (std::size_t index = 0; index < evidence.exceptional_cases.size();
       ++index) {
    if (index != 0) {
      out << ',';
    }
    const auto& exceptional = evidence.exceptional_cases[index];
    out << "{\"case_id\":\"" << json_escape(exceptional.case_id)
        << "\",\"worker_count\":2,\"input_waypoint_count\":8,"
           "\"control_condition\":\"";
    if (exceptional.case_id == "pre_cancelled") {
      out << "pre_cancelled_operator_stop";
    } else if (exceptional.case_id == "expired_absolute_deadline") {
      out << "already_expired_absolute_deadline_zero";
    } else {
      out << "worker_index_1_throws_std_exception";
    }
    out << "\",\"throwing_worker_index\":";
    if (exceptional.case_id == "worker_exception") {
      out << '1';
    } else {
      out << "null";
    }
    out << ",\"absolute_deadline_ns\":";
    if (exceptional.case_id == "expired_absolute_deadline") {
      out << '0';
    } else {
      out << "null";
    }
    out << ",\"pre_cancel_reason\":";
    if (exceptional.case_id == "pre_cancelled") {
      out << "\"operator_stop\"";
    } else {
      out << "null";
    }
    out
        << ",\"fallback_reason\":\""
        << json_escape(exceptional.fallback_reason)
        << "\",\"expected\":";
    if (exceptional.case_id == "pre_cancelled") {
      out << "{\"accepted\":false,\"action_code\":3,"
             "\"severity_code\":2,\"first_failing_waypoint\":0,"
             "\"first_failing_time_ns\":0,"
             "\"engine_status_code\":null,"
             "\"fallback_reason\":\"\","
             "\"reason\":\"operator_stop\"}";
    } else if (exceptional.case_id == "expired_absolute_deadline") {
      out << "{\"accepted\":false,\"action_code\":3,"
             "\"severity_code\":2,\"first_failing_waypoint\":0,"
             "\"first_failing_time_ns\":null,"
             "\"engine_status_code\":1,"
             "\"fallback_reason\":\"validation_wall_timeout\","
             "\"reason\":\"collision_validation_wall_timeout\"}";
    } else {
      out << "{\"accepted\":false,\"action_code\":3,"
             "\"severity_code\":2,\"first_failing_waypoint\":4,"
             "\"first_failing_time_ns\":40000000,"
             "\"engine_status_code\":5,"
             "\"fallback_reason\":\"\","
             "\"reason\":\"collision_validation_worker_exception\"}";
    }
    out << ",\"observed\":";
    append_precise_compact_bundle(
        out, exceptional.bundle,
        exceptional.case_id == "expired_absolute_deadline");
    out << '}';
  }
  out << "]}";
}

const char* mechanism_mode_for(const std::string& arm) {
  if (arm == "current_all_active_distance") {
    return "all_active_pairs";
  }
  if (arm == "signed_penetration_selective" ||
      arm == "bounded_penetration") {
    return "selected_colliding_pairs";
  }
  return "none";
}

void append_policy_fields(std::ostringstream& out,
                          const CollisionPolicySnapshot& policy,
                          const std::string& arm) {
  out << "{\"support_contact_count\":" << policy.support_contacts.size()
      << ",\"allowed_touch_pair_count\":"
      << policy.allowed_touch_link_pairs.size()
      << ",\"require_full_diagnostics\":"
      << (policy.require_full_diagnostics ? "true" : "false");
  if (policy.support_contacts.empty()) {
    out << ",\"mode\":null,\"release_evidence_mode\":null"
        << ",\"enforce_max_start_penetration\":null"
        << ",\"max_start_penetration_m\":null"
        << ",\"release_tolerance_m\":null"
        << ",\"min_release_progress_m\":null"
        << ",\"require_separation_by_end\":null"
        << ",\"require_strictly_decreasing_penetration\":null"
        << ",\"allow_recontact\":null"
        << ",\"max_contact_penetration_m\":null"
        << ",\"bounded_initial_contact_whitelist\":null}";
    return;
  }
  // The established object/support pair receives the metric modality. Exact
  // incidental gripper/support contacts from the unmodified fixture geometry
  // are represented separately by allowed_touch_link_pairs.
  const auto& entry = policy.support_contacts.front();
  out << ",\"mode\":\""
      << (arm == "bounded_penetration" ? "bounded_penetration"
                                         : "contact_release")
      << "\",\"release_evidence_mode\":";
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
  if (entry.mode == CollisionContactPolicyMode::kBoundedPenetration) {
    out << "null";
  } else {
    out << "\""
        << (entry.release_evidence_mode ==
                    ContactReleaseEvidenceMode::kBinaryContact
                ? "binary_contact"
                : "signed_penetration")
        << "\"";
  }
#else
  out << "null";
#endif
  out << ",\"enforce_max_start_penetration\":"
      << (entry.enforce_max_start_penetration ? "true" : "false")
      << ",\"max_start_penetration_m\":" << entry.max_start_penetration_m
      << ",\"release_tolerance_m\":" << entry.release_tolerance_m
      << ",\"min_release_progress_m\":" << entry.min_release_progress_m
      << ",\"require_separation_by_end\":"
      << (entry.require_separation_by_end ? "true" : "false")
      << ",\"require_strictly_decreasing_penetration\":"
      << (entry.require_strictly_decreasing_penetration ? "true" : "false")
      << ",\"allow_recontact\":"
      << (entry.allow_recontact ? "true" : "false")
      << ",\"max_contact_penetration_m\":";
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
  if (entry.mode == CollisionContactPolicyMode::kBoundedPenetration) {
    out << entry.max_contact_penetration_m;
  } else {
    out << "null";
  }
#else
  out << "null";
#endif
  out << ",\"bounded_initial_contact_whitelist\":";
  if (arm == "bounded_penetration") {
    // Bounded penetration applies to the configured pair whenever it becomes
    // colliding; it is deliberately independent of initial contact state.
    out << "false";
  } else {
    out << "null";
  }
  out << "}";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    const auto process_environment = observe_process_environment();
    if (process_environment.effective_cpu_affinity !=
        options.requested_cpu_affinity) {
      throw std::runtime_error(
          "effective CPU affinity differs from the canonical requested set");
    }
    if (process_environment.kernel_id != options.host_kernel_id) {
      throw std::runtime_error(
          "observed kernel identity differs from runner provenance");
    }
    if (options.precise_reference_only) {
      auto reference_snapshot = std::make_shared<CollisionWorldSnapshot>();
      reference_snapshot->revision.revision_id = 1;
      reference_snapshot->revision.content_hash =
          "step18_precise_reference_snapshot_v1";
      reference_snapshot->snapshot_format_version = 1;
      const auto evidence =
          collect_precise_reference_parity(reference_snapshot);
      const bool shape_ok =
          evidence.small_case_profiles.size() == 5 &&
          std::all_of(evidence.small_case_profiles.begin(),
                      evidence.small_case_profiles.end(),
                      [](const auto& profile) {
                        return profile.size() == 10;
                      }) &&
          evidence.partition_profiles.size() == 5 &&
          std::all_of(evidence.partition_profiles.begin(),
                      evidence.partition_profiles.end(),
                      [](const auto& profile) {
                        return profile.outcomes.size() == 1024;
                      }) &&
          evidence.completion_orders.size() == 6 &&
          evidence.exceptional_cases.size() == 3;
      std::ostringstream out;
      out << "{\"schema_version\":"
             "\"manyforge.step18.precise_reference_parity.v1\","
             "\"record_type\":\"precise_reference_parity\","
             "\"passed\":"
          << (shape_ok ? "true" : "false")
          << ",\"timing_gate_excluded\":true"
          << ",\"measurement_profile_usage\":"
             "\"provenance_only_no_timing_samples\""
          << ",\"source_role\":\"" << json_escape(options.source_role)
          << "\",\"source_commit\":\""
          << json_escape(options.source_commit)
          << "\",\"specs_commit\":\""
          << json_escape(options.specs_commit)
          << "\",\"specs_candidate_sha256\":\""
          << json_escape(options.specs_candidate_sha256)
          << "\",\"working_tree_candidate_sha256\":\""
          << json_escape(options.working_tree_candidate_sha256)
          << "\",\"image_id\":\"" << json_escape(options.image_id)
          << "\",\"binary_sha256\":\""
          << json_escape(options.binary_sha256)
          << "\",\"run_id\":\"" << json_escape(options.run_id)
          << "\",\"requested_cpu_affinity\":\""
          << json_escape(options.requested_cpu_affinity)
          << "\",\"effective_cpu_affinity\":\""
          << json_escape(process_environment.effective_cpu_affinity)
          << "\",\"scheduler_policy\":\""
          << json_escape(process_environment.scheduler_policy)
          << "\",\"scheduler_priority\":"
          << process_environment.scheduler_priority
          << ",\"process_nice\":" << process_environment.process_nice
          << ",\"kernel_id\":\""
          << json_escape(process_environment.kernel_id)
          << "\",\"cpu_governor_set\":\""
          << json_escape(options.cpu_governor_set)
          << "\",\"cpu_frequency_driver_set\":\""
          << json_escape(options.cpu_frequency_driver_set)
          << "\",\"clean_checkout_manifest_sha256\":";
      if (options.clean_checkout_manifest_sha256.empty()) {
        out << "null";
      } else {
        out << '\"' << json_escape(options.clean_checkout_manifest_sha256)
            << '\"';
      }
      out << ",\"qualification_patch_sha256\":";
      if (options.qualification_patch_sha256.empty()) {
        out << "null";
      } else {
        out << '\"' << json_escape(options.qualification_patch_sha256)
            << '\"';
      }
      out << ",\"compiler\":\"" << json_escape(__VERSION__) << "\""
          << ",\"compiler_flags\":\""
          << json_escape(options.compiler_flags) << "\""
#ifdef NDEBUG
          << ",\"build_type\":\"Release\""
#else
          << ",\"build_type\":\"non_release\""
#endif
          << ",\"warmup_runs\":" << options.warmup_runs
          << ",\"measured_runs\":" << options.measured_runs
          << ",\"corpus_sha256\":\""
          << json_escape(options.corpus_sha256)
          << "\",\"reference_evidence\":";
      append_precise_reference_parity_evidence(out, evidence);
      out << '}';
      std::cout << out.str() << '\n';
      return shape_ok ? 0 : 1;
    }
    auto native = create_native_kernel("ur5_gripper", "arm");
    if (!native || !native->scene_authority) {
      throw std::runtime_error("failed to create native kernel fixture");
    }
    auto state = observed_state(*native);
    const std::vector<std::string> established_joint_names{
        "shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
        "wrist_1_joint", "wrist_2_joint", "wrist_3_joint"};
    const std::vector<double> established_positions{
        0.0, -0.5, 0.5, -0.5, -0.5, 0.0};
    if (state.joint_names != established_joint_names ||
        state.joint_names.size() != established_positions.size()) {
      throw std::runtime_error(
          "established support fixture joint order is unavailable");
    }
    state.positions_rad = established_positions;
    FkRequestDTO fk_request;
    fk_request.robot_state = state;
    fk_request.target_frame = "tool0";
    fk_request.base_frame = "base_link";
    const auto tcp_pose = native->kinematics->forward_kinematics(fk_request);
    if (!tcp_pose.ok()) {
      throw std::runtime_error("failed to place support envelope: " +
                               tcp_pose.error().message);
    }
    const auto initial_snapshot = native->scene_authority->snapshot();
    if (!initial_snapshot) {
      throw std::runtime_error("fixture published no collision snapshot");
    }
    const auto snapshot =
        build_established_snapshot(*initial_snapshot, tcp_pose.value());
    const auto discovery = discover_contacts(snapshot, state);
    const auto trajectory =
        repeated_trajectory(state, options.waypoint_count);
    const auto policy = make_policy(
        options.arm, discovery.metric_support_pairs,
        discovery.physical_collision_pairs);

    std::vector<std::shared_ptr<ICollisionQueryEngine>> engines;
    engines.reserve(static_cast<std::size_t>(options.worker_count));
    for (int index = 0; index < options.worker_count; ++index) {
      auto engine = make_engine();
      engines.push_back(std::move(engine));
    }

    ParallelCollisionValidationWorkerPool::Config pool_config;
    pool_config.worker_count = options.worker_count;
    pool_config.min_waypoints_for_parallel = 1;
    pool_config.pass_config.policy_snapshot = policy;
    // This cell deliberately times a repeated-contact plateau. It exercises
    // policy evaluation without adding a release/progress/end obligation.
    pool_config.pass_config.enforce_contact_release_trajectory_end = false;
    ParallelCollisionValidationWorkerPool pool(pool_config);
    const auto snapshot_owner =
        std::make_shared<const manyforge::core::CollisionWorldSnapshot>(
            snapshot);

    std::optional<OutputSummary> reference;
    auto run_once = [&]() {
      auto cancellation = std::make_shared<CollisionCancellationToken>();
      const auto bundle = pool.validate_trajectory(
          snapshot_owner, engines, trajectory, cancellation,
          pool_config.pass_config);
      const auto summary =
          summarize(bundle, discovery.physical_collision_pairs);
      const auto expected_metric_observations =
          discovery.metric_support_pairs.size() * trajectory.waypoints.size();
      const auto expected_physical_observations =
          discovery.physical_collision_pairs.size() *
          trajectory.waypoints.size();
      bool clearance_scope_ok = false;
      if (options.arm == "current_all_active_distance") {
        clearance_scope_ok =
            summary.clearance_observation_count > expected_metric_observations &&
            summary.last_waypoint_clearance_count >
                summary.last_waypoint_pair_count;
      } else if (options.arm == "signed_penetration_selective" ||
                 options.arm == "bounded_penetration") {
        clearance_scope_ok =
            summary.clearance_observation_count == expected_metric_observations &&
            summary.finite_colliding_clearance_observation_count ==
                expected_metric_observations;
      } else {
        clearance_scope_ok = summary.clearance_observation_count == 0 &&
                             summary.last_waypoint_clearance_count == 0;
      }
      if (!summary.accepted || !summary.all_engine_status_ok ||
          !summary.every_waypoint_in_collision ||
          summary.waypoint_record_count != trajectory.waypoints.size() ||
          summary.normalized_pair_observation_count !=
              expected_physical_observations ||
          !clearance_scope_ok) {
        throw std::runtime_error(
            "policy-level support trajectory did not exercise its exact scope");
      }
      if (reference &&
          (reference->normalized_pair_output_fingerprint !=
               summary.normalized_pair_output_fingerprint ||
           reference->normalized_pair_observation_count !=
               summary.normalized_pair_observation_count ||
           reference->accepted != summary.accepted ||
           reference->action != summary.action ||
           reference->severity != summary.severity ||
           reference->first_failing_waypoint !=
               summary.first_failing_waypoint ||
           reference->first_failing_time_ns !=
               summary.first_failing_time_ns)) {
        throw std::runtime_error(
            "policy-level physical output changed across identical runs");
      }
      reference = summary;
    };

#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
    bool bounded_negative_checked = false;
    bool bounded_negative_rejected = false;
    std::optional<std::int64_t> bounded_negative_first_failure;
    double bounded_negative_cap_m = 0.0;
    if (options.arm == "bounded_penetration") {
      bounded_negative_checked = true;
      bounded_negative_cap_m =
          discovery.maximum_support_penetration_m * 0.5;
      auto negative_policy = make_policy(
          options.arm, discovery.metric_support_pairs,
          discovery.physical_collision_pairs, bounded_negative_cap_m);
      ParallelCollisionValidationWorkerPool::Config negative_config;
      negative_config.worker_count = 1;
      negative_config.min_waypoints_for_parallel = 1;
      negative_config.pass_config.policy_snapshot =
          std::move(negative_policy);
      ParallelCollisionValidationWorkerPool negative_pool(negative_config);
      std::vector<std::shared_ptr<ICollisionQueryEngine>> negative_engines{
          make_engine()};
      const auto negative_trajectory = repeated_trajectory(state, 1);
      auto negative_cancel =
          std::make_shared<CollisionCancellationToken>();
      const auto negative = negative_pool.validate_trajectory(
          snapshot_owner, negative_engines, negative_trajectory,
          negative_cancel, negative_config.pass_config);
      bounded_negative_rejected = !negative.decision.accepted;
      bounded_negative_first_failure =
          negative.decision.first_failing_waypoint;
      if (!bounded_negative_rejected ||
          bounded_negative_first_failure != std::optional<std::int64_t>{0}) {
        throw std::runtime_error(
            "bounded threshold-exceed negative correctness smoke did not reject");
      }
    }
#endif

    for (int index = 0; index < options.warmup_runs; ++index) {
      run_once();
    }
    std::vector<std::int64_t> wall_samples_ns;
    std::vector<std::int64_t> cpu_samples_ns;
    wall_samples_ns.reserve(options.measured_runs);
    cpu_samples_ns.reserve(options.measured_runs);
    for (int index = 0; index < options.measured_runs; ++index) {
      const auto wall_started = std::chrono::steady_clock::now();
      const auto cpu_started = process_cpu_time_ns();
      run_once();
      const auto cpu_finished = process_cpu_time_ns();
      const auto wall_finished = std::chrono::steady_clock::now();
      cpu_samples_ns.push_back(cpu_finished - cpu_started);
      wall_samples_ns.push_back(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              wall_finished - wall_started)
              .count());
    }
    if (!reference) {
      throw std::runtime_error("policy-level benchmark produced no output");
    }

    const auto telemetry = pool.last_telemetry();

    // Collect counters in one complete, independent trajectory pass after all
    // timing. Counter observation cannot perturb the measured samples, while
    // every waypoint remains represented in the reported totals.
    std::vector<std::shared_ptr<FullTrajectoryObservationEngine>>
        observation_engines;
    std::vector<std::shared_ptr<ICollisionQueryEngine>>
        observation_engine_interfaces;
    observation_engines.reserve(static_cast<std::size_t>(options.worker_count));
    observation_engine_interfaces.reserve(
        static_cast<std::size_t>(options.worker_count));
    for (int index = 0; index < options.worker_count; ++index) {
      auto observation = std::make_shared<FullTrajectoryObservationEngine>(
          make_engine(), options.arm, discovery.metric_support_pairs.size());
      observation_engines.push_back(observation);
      observation_engine_interfaces.push_back(std::move(observation));
    }
    ParallelCollisionValidationWorkerPool observation_pool(pool_config);
    auto observation_cancel =
        std::make_shared<CollisionCancellationToken>();
    const auto observation_bundle = observation_pool.validate_trajectory(
        snapshot_owner, observation_engine_interfaces, trajectory,
        observation_cancel, pool_config.pass_config);
    const auto observation_summary =
        summarize(observation_bundle, discovery.physical_collision_pairs);
    const auto observation_telemetry = observation_pool.last_telemetry();
    if (observation_summary.normalized_pair_output_fingerprint !=
            reference->normalized_pair_output_fingerprint ||
        observation_summary.normalized_pair_observation_count !=
            reference->normalized_pair_observation_count ||
        observation_summary.accepted != reference->accepted ||
        observation_summary.action != reference->action ||
        observation_summary.severity != reference->severity ||
        observation_summary.first_failing_waypoint !=
            reference->first_failing_waypoint ||
        observation_summary.first_failing_time_ns !=
            reference->first_failing_time_ns) {
      throw std::runtime_error(
          "mechanism-observation pass changed physical validation output");
    }

    MechanismTotals mechanism;
    std::size_t queried_engine_count = 0;
    for (const auto& engine : observation_engines) {
      const auto& totals = engine->totals();
      if (totals.query_count == 0) {
        continue;
      }
      ++queried_engine_count;
      mechanism.query_count += totals.query_count;
      mechanism.collision_api_call_count += totals.collision_api_call_count;
      mechanism.active_collision_pair_count +=
          totals.active_collision_pair_count;
      mechanism.colliding_geometry_pair_count +=
          totals.colliding_geometry_pair_count;
      mechanism.colliding_logical_pair_count +=
          totals.colliding_logical_pair_count;
      mechanism.distance_api_call_count += totals.distance_api_call_count;
      mechanism.distance_evaluated_geometry_pair_count +=
          totals.distance_evaluated_geometry_pair_count;
      mechanism.distance_evaluated_logical_pair_count +=
          totals.distance_evaluated_logical_pair_count;
      mechanism.fk_placement_invocation_count +=
          totals.fk_placement_invocation_count;
      mechanism.exact_scope_matches =
          mechanism.exact_scope_matches && totals.exact_scope_matches;
    }
    const auto expected_physical_observations =
        discovery.physical_collision_pairs.size() *
        trajectory.waypoints.size();
    const auto expected_metric_observations =
        discovery.metric_support_pairs.size() * trajectory.waypoints.size();
    const std::uint64_t expected_fk_count =
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
        trajectory.waypoints.size();
#else
        2 * trajectory.waypoints.size();
#endif
    bool mechanism_matches =
        mechanism.exact_scope_matches &&
        mechanism.query_count == trajectory.waypoints.size() &&
        mechanism.collision_api_call_count == trajectory.waypoints.size() &&
        mechanism.colliding_logical_pair_count ==
            expected_physical_observations &&
        mechanism.fk_placement_invocation_count == expected_fk_count &&
        queried_engine_count > 0 &&
        queried_engine_count == static_cast<std::size_t>(
                                    observation_telemetry.workers_actually_used);
    if (options.arm == "current_all_active_distance") {
      mechanism_matches =
          mechanism_matches &&
          mechanism.distance_evaluated_logical_pair_count ==
              reference->clearance_observation_count &&
          mechanism.distance_evaluated_logical_pair_count >
              mechanism.colliding_logical_pair_count &&
          mechanism.distance_api_call_count == trajectory.waypoints.size();
    } else if (options.arm == "signed_penetration_selective" ||
               options.arm == "bounded_penetration") {
      mechanism_matches =
          mechanism_matches &&
          mechanism.distance_evaluated_logical_pair_count ==
              expected_metric_observations;
    } else {
      mechanism_matches =
          mechanism_matches && mechanism.distance_api_call_count == 0 &&
          mechanism.distance_evaluated_logical_pair_count == 0;
    }

    const bool passed = mechanism_matches;
    const auto state_hash = state_fingerprint(state);
    const auto snapshot_hash = snapshot_fingerprint(snapshot);
    const auto trajectory_hash = trajectory_fingerprint(trajectory);
    const auto pairs_hash =
        pair_fingerprint(discovery.physical_collision_pairs);
    const auto metric_pairs_hash =
        pair_fingerprint(discovery.metric_support_pairs);

    std::ostringstream out;
    out << "{\"schema_version\":\"manyforge.step18.policy_support_perf.v1\""
        << ",\"record_type\":\"policy_support_contact_validation\""
        << ",\"passed\":" << (passed ? "true" : "false")
        << ",\"arm\":\"" << json_escape(options.arm) << "\""
        << ",\"source_role\":\"" << json_escape(options.source_role)
        << "\",\"source_commit\":\"" << json_escape(options.source_commit)
        << "\",\"specs_commit\":\"" << json_escape(options.specs_commit)
        << "\",\"specs_candidate_sha256\":\""
        << json_escape(options.specs_candidate_sha256)
        << "\",\"image_id\":\"" << json_escape(options.image_id)
        << "\",\"working_tree_candidate_sha256\":\""
        << json_escape(options.working_tree_candidate_sha256)
        << "\",\"binary_sha256\":\""
        << json_escape(options.binary_sha256) << "\",\"run_id\":\""
        << json_escape(options.run_id)
        << "\",\"requested_cpu_affinity\":\""
        << json_escape(options.requested_cpu_affinity)
        << "\",\"effective_cpu_affinity\":\""
        << json_escape(process_environment.effective_cpu_affinity)
        << "\",\"scheduler_policy\":\""
        << json_escape(process_environment.scheduler_policy)
        << "\",\"scheduler_priority\":"
        << process_environment.scheduler_priority
        << ",\"process_nice\":" << process_environment.process_nice
        << ",\"kernel_id\":\""
        << json_escape(process_environment.kernel_id)
        << "\",\"cpu_governor_set\":\""
        << json_escape(options.cpu_governor_set)
        << "\",\"cpu_frequency_driver_set\":\""
        << json_escape(options.cpu_frequency_driver_set)
        << "\",\"clean_checkout_manifest_sha256\":";
    if (options.clean_checkout_manifest_sha256.empty()) {
      out << "null";
    } else {
      out << "\"" << json_escape(options.clean_checkout_manifest_sha256)
          << "\"";
    }
    out << ",\"qualification_patch_sha256\":";
    if (options.qualification_patch_sha256.empty()) {
      out << "null";
    } else {
      out << "\"" << json_escape(options.qualification_patch_sha256)
          << "\"";
    }
    out << ",\"compiler\":\"" << json_escape(__VERSION__) << "\""
        << ",\"compiler_flags\":\""
        << json_escape(options.compiler_flags) << "\""
#ifdef NDEBUG
        << ",\"build_type\":\"Release\""
#else
        << ",\"build_type\":\"non_release\""
#endif
        << ",\"fixture_id\":\"ur5_support_contact_release\""
        << ",\"fixture_variant\":\"dense_far_pair_distractor_extension_v1\""
        << ",\"fixture_world_profile\":\"support_contact\""
        << ",\"corpus_sha256\":\""
        << json_escape(options.corpus_sha256) << "\""
        << ",\"snapshot_content_hash\":\""
        << json_escape(snapshot.revision.content_hash) << "\""
        << ",\"snapshot_geometry_fingerprint\":\"" << snapshot_hash
        << "\",\"state_fingerprint\":\"" << state_hash
        << "\",\"trajectory_fingerprint\":\"" << trajectory_hash
        << "\",\"configured_pair_fingerprint\":\"" << metric_pairs_hash
        << "\",\"physical_collision_pair_fingerprint\":\"" << pairs_hash
        << "\",\"body_count\":" << snapshot.body_count
        << ",\"geometry_count\":" << snapshot.geometry_count
        << ",\"distractor_count\":" << kDistractorCount
        << ",\"configured_contact_pair_count\":"
        << discovery.metric_support_pairs.size()
        << ",\"physical_collision_pair_count\":"
        << discovery.physical_collision_pairs.size()
        << ",\"incidental_allowed_pair_count\":"
        << (discovery.physical_collision_pairs.size() -
            discovery.metric_support_pairs.size())
        << ",\"policy_id\":\"" << json_escape(policy.policy_id) << "\""
        << ",\"policy_content_hash\":\""
        << json_escape(policy.revision.content_hash) << "\""
        << ",\"policy_fields\":";
    append_policy_fields(out, policy, options.arm);
    out
        << ",\"discovered_maximum_penetration_m\":"
        << std::setprecision(17)
        << discovery.maximum_support_penetration_m
        << ",\"positive_penetration_cap_m\":"
        << kPositivePenetrationCapM
        << ",\"waypoint_count\":" << options.waypoint_count
        << ",\"worker_count_requested\":" << options.worker_count
        << ",\"worker_count_effective\":" << telemetry.workers_effective
        << ",\"workers_used\":" << telemetry.workers_actually_used
        << ",\"fallback_reason\":\""
        << json_escape(telemetry.fallback_reason) << "\""
        << ",\"warmup_runs\":" << options.warmup_runs
        << ",\"measured_runs\":" << options.measured_runs
        << ",\"wall_us\":";
    append_latency(out, wall_samples_ns);
    out << ",\"process_cpu_us\":";
    append_latency(out, cpu_samples_ns);
    out << ",\"decision_accepted\":"
        << (reference->accepted ? "true" : "false")
        << ",\"decision_action_code\":"
        << static_cast<int>(reference->action)
        << ",\"decision_severity_code\":"
        << static_cast<int>(reference->severity)
        << ",\"first_failing_waypoint\":";
    append_optional_integer_json(out, reference->first_failing_waypoint);
    out << ",\"first_failing_time_ns\":";
    append_optional_integer_json(out, reference->first_failing_time_ns);
    out
        << ",\"every_waypoint_in_collision\":"
        << (reference->every_waypoint_in_collision ? "true" : "false")
        << ",\"waypoint_record_count\":"
        << reference->waypoint_record_count
        << ",\"normalized_pair_observation_count\":"
        << reference->normalized_pair_observation_count
        << ",\"clearance_observation_count\":"
        << reference->clearance_observation_count
        << ",\"finite_colliding_clearance_observation_count\":"
        << reference->finite_colliding_clearance_observation_count
        << ",\"maximum_observed_penetration_m\":"
        << reference->maximum_observed_penetration_m
        << ",\"normalized_pair_output_fingerprint\":\""
        << reference->normalized_pair_output_fingerprint
        << "\",\"colliding_clearance_output_fingerprint\":\""
        << reference->colliding_clearance_output_fingerprint
        << "\""
        << ",\"mechanism_provenance\":\""
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
        << "direct_current_full_trajectory_observation"
#else
        << "exact_result_and_source_derivation_pristine_2bfe30e"
#endif
        << "\",\"distance_request_mode\":\""
        << mechanism_mode_for(options.arm) << "\""
        << ",\"mechanism_checked\":"
        << (mechanism_matches ? "true" : "false")
        << ",\"mechanism_timing_isolated\":true"
        << ",\"mechanism_query_count\":" << mechanism.query_count
        << ",\"queried_engine_count\":" << queried_engine_count
        << ",\"mechanism_workers_used\":"
        << observation_telemetry.workers_actually_used
        << ",\"collision_api_call_count\":"
        << mechanism.collision_api_call_count
        << ",\"active_collision_pair_count\":"
        << mechanism.active_collision_pair_count
        << ",\"colliding_geometry_pair_count\":"
        << mechanism.colliding_geometry_pair_count;
    out << ",\"colliding_logical_pair_count\":"
        << mechanism.colliding_logical_pair_count
        << ",\"distance_api_call_count\":"
        << mechanism.distance_api_call_count
        << ",\"distance_evaluated_geometry_pair_count\":"
        << mechanism.distance_evaluated_geometry_pair_count
        << ",\"distance_evaluated_logical_pair_count\":"
        << mechanism.distance_evaluated_logical_pair_count
        << ",\"fk_placement_invocation_count\":"
        << mechanism.fk_placement_invocation_count
        << ",\"fk_placement_provenance\":\""
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
        << "direct_current_full_trajectory_counter"
#else
        << "exact_pristine_source_derivation_collision_plus_bulk_distance"
#endif
        << "\""
        << ",\"fk_placement_derivation_source\":";
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
    out << "null";
#else
    out << "\"manyforge_collision/src/manyforge_collision_engine.cpp"
           "@2bfe30eeb37952f1ddc36bfdbf89d471a9f6b909:1221,1231\"";
#endif
    out
        << ",\"allocation_count\":null"
        << ",\"allocation_observation_status\":\"not_observed_in_this_timing_cell\""
        << ",\"snapshot_sync_time_us\":null"
        << ",\"snapshot_sync_observation_status\":\"not_applicable_immutable_prebuilt_snapshot\""
        << ",\"cancellation_status\":\"not_requested\""
        << ",\"timeout_status\":\"not_configured\""
        << ",\"bounded_negative_checked\":";
#if MANYFORGE_POLICY_SUPPORT_CURRENT_SOURCE
    out << (bounded_negative_checked ? "true" : "false")
        << ",\"bounded_negative_cap_m\":";
    if (bounded_negative_checked) {
      out << bounded_negative_cap_m;
    } else {
      out << "null";
    }
    out << ",\"bounded_negative_rejected\":";
    if (bounded_negative_checked) {
      out << (bounded_negative_rejected ? "true" : "false");
    } else {
      out << "null";
    }
    out << ",\"bounded_negative_first_failing_waypoint\":";
    if (bounded_negative_first_failure) {
      out << *bounded_negative_first_failure;
    } else {
      out << "null";
    }
#else
    out << "false,\"bounded_negative_cap_m\":null"
        << ",\"bounded_negative_rejected\":null"
        << ",\"bounded_negative_first_failing_waypoint\":null";
#endif
    out << "}";
    std::cout << out.str() << '\n';
    return passed ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "perf_policy_support_contact_qualification: "
              << error.what() << '\n';
    return 2;
  }
}
