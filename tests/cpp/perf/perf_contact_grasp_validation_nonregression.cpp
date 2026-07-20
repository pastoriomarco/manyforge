#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_core/types/scene_edit.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <roboplan_example_models/resources.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE
#define MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE 0
#endif

namespace {

using manyforge::collision::ManyForgeCollisionEngine;
using manyforge::core::AddObjectRequestDTO;
using manyforge::core::CollisionCancellationToken;
using manyforge::core::CollisionEngineStatus;
using manyforge::core::CollisionPolicySnapshot;
using manyforge::core::CollisionShapeDTO;
using manyforge::core::ContactGraspPolicyEntry;
using manyforge::core::CylinderShapeDTO;
using manyforge::core::FkRequestDTO;
using manyforge::core::ICollisionQueryEngine;
using manyforge::core::ParallelCollisionValidationWorkerPool;
using manyforge::core::Pose3DTO;
using manyforge::core::RobotStateDTO;
using manyforge::core::TrajectoryDTO;
using manyforge::core::TrajectoryWaypointDTO;
using manyforge::roboplan_backend::NativeKernelBundle;
using manyforge::roboplan_backend::create_native_kernel;

constexpr std::string_view kObjectId = "step18_contact_grasp_envelope";
constexpr std::string_view kPolicyId = "contact_grasp/step18_envelope";
constexpr double kEnvelopeRadiusM = 0.15;
constexpr double kEnvelopeHeightM = 0.30;
constexpr double kMaxPenetrationM = 0.50;
constexpr double kPi = 3.14159265358979323846;

struct Options {
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
    if (option == "--waypoints") {
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
          << "usage: perf_contact_grasp_validation_nonregression "
             "--waypoints N --workers N --warmup-runs N --measured-runs N\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + option);
    }
  }
  if (options.waypoint_count < 2) {
    throw std::invalid_argument(
        "--waypoints must include one clear start and one contact sample");
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

RobotStateDTO observed_state(const NativeKernelBundle& bundle) {
  auto state =
      bundle.observed_state_provider->current_state(bundle.clock->now());
  if (!state.ok()) {
    throw std::runtime_error("observed robot state is unavailable: " +
                             state.error().message);
  }
  return std::move(state.value());
}

std::vector<std::string> robot_link_names(
    const manyforge::core::CollisionWorldSnapshot& world) {
  constexpr std::string_view marker = "/link/";
  std::vector<std::string> links;
  for (const auto& body : world.bodies) {
    if (body.body_id.rfind("robot/ur5_gripper/", 0) != 0) {
      continue;
    }
    const auto marker_at = body.body_id.find(marker);
    if (marker_at == std::string::npos) {
      continue;
    }
    const auto name_begin = marker_at + marker.size();
    const auto name_end = body.body_id.find('/', name_begin);
    if (name_end == std::string::npos || name_end == name_begin) {
      continue;
    }
    links.push_back(body.body_id.substr(name_begin, name_end - name_begin));
  }
  std::sort(links.begin(), links.end());
  links.erase(std::unique(links.begin(), links.end()), links.end());
  if (links.empty()) {
    throw std::runtime_error("contact-grasp fixture resolved no robot links");
  }
  return links;
}

TrajectoryDTO repeated_trajectory(const RobotStateDTO& state,
                                  int waypoint_count) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = state.joint_names;
  trajectory.waypoints.reserve(static_cast<std::size_t>(waypoint_count));
  for (int index = 0; index < waypoint_count; ++index) {
    TrajectoryWaypointDTO waypoint;
    waypoint.positions_rad = state.positions_rad;
    if (index == 0) {
      waypoint.positions_rad.front() += kPi;
    }
    waypoint.time_from_start_s = static_cast<double>(index) * 0.01;
    trajectory.waypoints.push_back(std::move(waypoint));
  }
  return trajectory;
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
  const double rank =
      fraction * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(rank);
  const auto upper = std::min(lower + 1, values.size() - 1);
  const double weight = rank - static_cast<double>(lower);
  return (static_cast<double>(values[lower]) * (1.0 - weight) +
          static_cast<double>(values[upper]) * weight) /
         1000.0;
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

  std::string finish() const {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value_;
    return out.str();
  }

 private:
  std::uint64_t value_ = 1469598103934665603ULL;
};

struct OutputSummary {
  bool accepted = false;
  bool all_engine_status_ok = true;
  bool initial_waypoint_clear = true;
  bool every_contact_waypoint_in_collision = true;
  std::size_t waypoint_record_count = 0;
  std::size_t normalized_pair_observation_count = 0;
  std::size_t clearance_observation_count = 0;
  std::size_t initial_waypoint_clearance_count = 0;
  std::size_t finite_colliding_clearance_observation_count = 0;
  bool every_colliding_pair_has_finite_clearance = true;
  double maximum_observed_penetration_m = 0.0;
  std::string normalized_pair_output_fingerprint;
  std::string colliding_clearance_output_fingerprint;
};

OutputSummary summarize(
    const manyforge::core::CollisionValidationDecisionBundle& bundle) {
  OutputSummary summary;
  summary.accepted = bundle.decision.accepted;
  summary.waypoint_record_count = bundle.waypoint_records.size();
  Fingerprint fingerprint;
  Fingerprint clearance_fingerprint;
  fingerprint.mix(static_cast<std::uint64_t>(bundle.decision.accepted));
  fingerprint.mix(static_cast<std::uint64_t>(bundle.decision.action));
  fingerprint.mix(static_cast<std::uint64_t>(bundle.decision.severity));
  fingerprint.mix(bundle.waypoint_records.size());
  for (const auto& record : bundle.waypoint_records) {
    summary.all_engine_status_ok =
        summary.all_engine_status_ok &&
        record.result.engine_status == CollisionEngineStatus::kOk;
    if (record.waypoint_index == 0) {
      summary.initial_waypoint_clear =
          summary.initial_waypoint_clear && !record.result.in_collision;
    } else {
      summary.every_contact_waypoint_in_collision =
          summary.every_contact_waypoint_in_collision &&
          record.result.in_collision;
    }
    summary.normalized_pair_observation_count +=
        record.result.normalized_pairs.size();
    summary.clearance_observation_count +=
        record.result.pair_clearances.size();
    if (record.waypoint_index == 0) {
      summary.initial_waypoint_clearance_count =
          record.result.pair_clearances.size();
    }
    fingerprint.mix(static_cast<std::uint64_t>(record.waypoint_index));
    fingerprint.mix(static_cast<std::uint64_t>(record.result.engine_status));
    fingerprint.mix(static_cast<std::uint64_t>(record.result.in_collision));
    fingerprint.mix(record.result.normalized_pairs.size());
    for (const auto& pair : record.result.normalized_pairs) {
      fingerprint.mix(pair.body_a);
      fingerprint.mix(pair.body_b);
      const auto clearance = std::find_if(
          record.result.pair_clearances.begin(),
          record.result.pair_clearances.end(), [&](const auto& candidate) {
            return candidate.pair == pair;
          });
      if (clearance == record.result.pair_clearances.end() ||
          !std::isfinite(clearance->signed_clearance_m) ||
          clearance->signed_clearance_m > 0.0) {
        summary.every_colliding_pair_has_finite_clearance = false;
        continue;
      }
      ++summary.finite_colliding_clearance_observation_count;
      summary.maximum_observed_penetration_m =
          std::max(summary.maximum_observed_penetration_m,
                   -clearance->signed_clearance_m);
      clearance_fingerprint.mix(
          static_cast<std::uint64_t>(record.waypoint_index));
      clearance_fingerprint.mix(pair.body_a);
      clearance_fingerprint.mix(pair.body_b);
      clearance_fingerprint.mix(
          std::bit_cast<std::uint64_t>(clearance->signed_clearance_m));
    }
  }
  summary.normalized_pair_output_fingerprint = fingerprint.finish();
  summary.colliding_clearance_output_fingerprint =
      clearance_fingerprint.finish();
  return summary;
}

void append_latency(std::ostringstream& out,
                    const std::vector<std::int64_t>& values) {
  out << std::fixed << std::setprecision(3)
      << "{\"p50\":" << percentile_us(values, 0.50)
      << ",\"p95\":" << percentile_us(values, 0.95)
      << ",\"p99\":" << percentile_us(values, 0.99)
      << ",\"max\":" << percentile_us(values, 1.0) << "}";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    auto native = create_native_kernel("ur5_gripper", "arm");
    if (!native || !native->scene_authority) {
      throw std::runtime_error("failed to create native kernel fixture");
    }
    const auto state = observed_state(*native);
    FkRequestDTO fk_request;
    fk_request.robot_state = state;
    fk_request.target_frame = "tool0";
    fk_request.base_frame = "base_link";
    const auto tcp_pose = native->kinematics->forward_kinematics(fk_request);
    if (!tcp_pose.ok()) {
      throw std::runtime_error("failed to place contact-grasp fixture: " +
                               tcp_pose.error().message);
    }

    AddObjectRequestDTO object;
    object.object_id = std::string(kObjectId);
    object.shape = CollisionShapeDTO{
        CylinderShapeDTO{kEnvelopeRadiusM, kEnvelopeHeightM}};
    object.pose = tcp_pose.value();
    object.frame_id = "universe";
    const auto added = native->scene_authority->add_object(object);
    if (!added.success) {
      throw std::runtime_error(
          added.error ? added.error->message
                      : "failed to add contact-grasp envelope");
    }
    const auto snapshot = native->scene_authority->snapshot();
    if (!snapshot) {
      throw std::runtime_error("fixture published no collision snapshot");
    }
    const auto links = robot_link_names(*snapshot);
    const auto trajectory = repeated_trajectory(state, options.waypoint_count);

    CollisionPolicySnapshot policy;
    policy.policy_id = std::string(kPolicyId);
    policy.revision.revision_id = 1;
    policy.revision.content_hash = "step18_contact_grasp_envelope_v1";
    ContactGraspPolicyEntry grasp;
    grasp.policy_id = std::string(kPolicyId);
    grasp.selected_object_id = std::string(kObjectId);
    grasp.allowed_robot_links = links;
    grasp.max_penetration_m = kMaxPenetrationM;
    grasp.terminal_window_start_fraction = 0.0;
    policy.contact_grasps.push_back(std::move(grasp));

    std::vector<std::shared_ptr<ICollisionQueryEngine>> engines;
#if MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE
    std::vector<std::shared_ptr<ManyForgeCollisionEngine>> concrete_engines;
#endif
    engines.reserve(static_cast<std::size_t>(options.worker_count));
#if MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE
    concrete_engines.reserve(static_cast<std::size_t>(options.worker_count));
#endif
    for (int index = 0; index < options.worker_count; ++index) {
      auto engine = make_engine();
#if MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE
      concrete_engines.push_back(engine);
#endif
      engines.push_back(std::move(engine));
    }

    ParallelCollisionValidationWorkerPool::Config pool_config;
    pool_config.worker_count = options.worker_count;
    pool_config.min_waypoints_for_parallel = 1;
    pool_config.pass_config.policy_snapshot = policy;
    pool_config.pass_config.contact_policy_id = std::string(kPolicyId);
    ParallelCollisionValidationWorkerPool pool(pool_config);
    const auto snapshot_owner =
        std::make_shared<const manyforge::core::CollisionWorldSnapshot>(
            *snapshot);

    std::optional<OutputSummary> reference;
    auto run_once = [&]() {
      auto cancellation = std::make_shared<CollisionCancellationToken>();
      auto pass_config = pool_config.pass_config;
      const auto bundle = pool.validate_trajectory(
          snapshot_owner, engines, trajectory, cancellation,
          std::move(pass_config));
      const auto summary = summarize(bundle);
      if (!summary.accepted || !summary.all_engine_status_ok ||
          !summary.initial_waypoint_clear ||
          !summary.every_contact_waypoint_in_collision ||
          summary.waypoint_record_count !=
              static_cast<std::size_t>(options.waypoint_count) ||
          summary.normalized_pair_observation_count == 0 ||
          summary.clearance_observation_count == 0 ||
          summary.finite_colliding_clearance_observation_count !=
              summary.normalized_pair_observation_count ||
          !summary.every_colliding_pair_has_finite_clearance
#if MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE
          || summary.initial_waypoint_clearance_count != 0
#endif
      ) {
        std::ostringstream error;
        error << "contact-grasp validation fixture did not exercise accepted "
                 "colliding metric evidence: accepted="
              << summary.accepted
              << " status_ok=" << summary.all_engine_status_ok
              << " initial_clear=" << summary.initial_waypoint_clear
              << " every_contact_waypoint_colliding="
              << summary.every_contact_waypoint_in_collision
              << " records=" << summary.waypoint_record_count
              << " normalized="
              << summary.normalized_pair_observation_count
              << " clearances=" << summary.clearance_observation_count
              << " initial_clearances="
              << summary.initial_waypoint_clearance_count
              << " finite_colliding="
              << summary.finite_colliding_clearance_observation_count
              << " max_penetration_m="
              << summary.maximum_observed_penetration_m;
        if (bundle.decision.first_failing_waypoint) {
          error << " first_failure="
                << *bundle.decision.first_failing_waypoint;
        }
        for (const auto& [key, value] :
             bundle.waypoint_records.front().result.raw_diagnostics) {
          error << " diagnostic[" << key << "]=" << value;
        }
        throw std::runtime_error(error.str());
      }
      if (reference &&
          (reference->normalized_pair_output_fingerprint !=
               summary.normalized_pair_output_fingerprint ||
           reference->normalized_pair_observation_count !=
               summary.normalized_pair_observation_count)) {
        throw std::runtime_error(
            "contact-grasp output changed across identical warm runs");
      }
      reference = summary;
    };

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

    const auto telemetry = pool.last_telemetry();
#if MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE
    bool mechanism_matches = true;
    std::size_t queried_engine_count = 0;
    std::uint64_t collision_api_call_count = 0;
    std::uint64_t active_collision_pair_count = 0;
    std::uint64_t colliding_geometry_pair_count = 0;
    std::uint64_t colliding_logical_pair_count = 0;
    std::uint64_t distance_api_call_count = 0;
    std::uint64_t distance_evaluated_geometry_pair_count = 0;
    std::uint64_t distance_evaluated_logical_pair_count = 0;
    for (const auto& engine : concrete_engines) {
      const auto counters = engine->internal_last_query_mechanism_counters();
      if (counters.collision_api_call_count == 0) {
        continue;
      }
      ++queried_engine_count;
      collision_api_call_count += counters.collision_api_call_count;
      active_collision_pair_count += counters.active_collision_pair_count;
      colliding_geometry_pair_count +=
          counters.colliding_geometry_pair_count;
      colliding_logical_pair_count += counters.colliding_logical_pair_count;
      distance_api_call_count += counters.distance_api_call_count;
      distance_evaluated_geometry_pair_count +=
          counters.distance_evaluated_geometry_pair_count;
      distance_evaluated_logical_pair_count +=
          counters.distance_evaluated_logical_pair_count;
      mechanism_matches =
          mechanism_matches &&
          counters.distance_request_mode == manyforge::core::
                                                CollisionDistanceRequestMode::
                                                    kSelectedCollidingPairs &&
          counters.collision_api_call_count == 1 &&
          counters.collision_enumeration_completion_ordinal == 1 &&
          counters.first_distance_evaluation_ordinal == 2 &&
          counters.active_collision_pair_count >
              counters.colliding_geometry_pair_count &&
          counters.colliding_geometry_pair_count > 0 &&
          counters.colliding_logical_pair_count > 0 &&
          counters.distance_api_call_count > 0 &&
          counters.distance_api_call_count ==
              counters.distance_evaluated_geometry_pair_count &&
          counters.distance_evaluated_geometry_pair_count ==
              counters.colliding_geometry_pair_count &&
          counters.distance_evaluated_logical_pair_count ==
              counters.colliding_logical_pair_count;
    }
    mechanism_matches =
        mechanism_matches && queried_engine_count > 0 &&
        queried_engine_count ==
            static_cast<std::size_t>(telemetry.workers_actually_used);
#endif

#if MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE
    const bool passed = reference.has_value() && mechanism_matches;
#else
    const bool passed = reference.has_value();
#endif
    std::ostringstream out;
    out << "{\"schema_version\":\"manyforge.step18.validation_nonregression.v1\""
        << ",\"record_type\":\"contact_grasp_validation\""
        << ",\"passed\":" << (passed ? "true" : "false")
        << ",\"source_role\":\"" << json_escape(options.source_role)
        << "\",\"source_commit\":\"" << json_escape(options.source_commit)
        << "\",\"specs_commit\":\"" << json_escape(options.specs_commit)
        << "\",\"specs_candidate_sha256\":\""
        << json_escape(options.specs_candidate_sha256)
        << "\",\"image_id\":\"" << json_escape(options.image_id)
        << "\",\"working_tree_candidate_sha256\":\""
        << json_escape(options.working_tree_candidate_sha256)
        << "\",\"binary_sha256\":\""
        << json_escape(options.binary_sha256)
        << "\",\"run_id\":\"" << json_escape(options.run_id)
        << "\",\"requested_cpu_affinity\":\""
        << json_escape(options.requested_cpu_affinity)
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
    out << ",\"fixture_id\":\"step18_contact_grasp_envelope_v1\""
        << ",\"waypoint_count\":" << options.waypoint_count
        << ",\"worker_count_requested\":" << options.worker_count
        << ",\"worker_count_effective\":"
        << telemetry.workers_effective
        << ",\"workers_used\":" << telemetry.workers_actually_used
        << ",\"fallback_reason\":\""
        << json_escape(telemetry.fallback_reason) << "\""
        << ",\"warmup_runs\":" << options.warmup_runs
        << ",\"measured_runs\":" << options.measured_runs
        << ",\"initial_waypoint_clear_by_construction\":true"
        << ",\"identical_contact_state_after_initial_waypoint\":true"
        << ",\"allowed_robot_link_count\":" << links.size()
        << ",\"max_penetration_m\":" << std::setprecision(17)
        << kMaxPenetrationM << ",\"wall_us\":";
    append_latency(out, wall_samples_ns);
    out << ",\"process_cpu_us\":";
    append_latency(out, cpu_samples_ns);
    out << ",\"decision_accepted\":"
        << (reference->accepted ? "true" : "false")
        << ",\"initial_waypoint_clear\":"
        << (reference->initial_waypoint_clear ? "true" : "false")
        << ",\"every_contact_waypoint_in_collision\":"
        << (reference->every_contact_waypoint_in_collision ? "true"
                                                          : "false")
        << ",\"waypoint_record_count\":"
        << reference->waypoint_record_count
        << ",\"normalized_pair_observation_count\":"
        << reference->normalized_pair_observation_count
        << ",\"clearance_observation_count\":"
        << reference->clearance_observation_count
        << ",\"initial_waypoint_clearance_count\":"
        << reference->initial_waypoint_clearance_count
        << ",\"finite_colliding_clearance_observation_count\":"
        << reference->finite_colliding_clearance_observation_count
        << ",\"maximum_observed_penetration_m\":"
        << std::setprecision(17)
        << reference->maximum_observed_penetration_m
        << ",\"normalized_pair_output_fingerprint\":\""
        << reference->normalized_pair_output_fingerprint << "\""
        << ",\"colliding_clearance_output_fingerprint\":\""
        << reference->colliding_clearance_output_fingerprint << "\""
        << ",\"mechanism_provenance\":\""
#if MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE
        << "direct_current"
#else
        << "not_available_pristine_timing"
#endif
        << "\",\"distance_request_mode\":";
#if MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE
    out << "\"selected_colliding_pairs\"";
#else
    out << "null";
#endif
    out << ",\"mechanism_checked\":";
#if MANYFORGE_CONTACT_GRASP_CURRENT_SOURCE
    out << "true"
        << ",\"queried_engine_count\":" << queried_engine_count
        << ",\"collision_api_call_count\":" << collision_api_call_count
        << ",\"collision_enumeration_completion_ordinal\":1"
        << ",\"first_distance_evaluation_ordinal\":2"
        << ",\"active_collision_pair_count\":"
        << active_collision_pair_count
        << ",\"colliding_geometry_pair_count\":"
        << colliding_geometry_pair_count
        << ",\"colliding_logical_pair_count\":"
        << colliding_logical_pair_count
        << ",\"distance_api_call_count\":" << distance_api_call_count
        << ",\"distance_evaluated_geometry_pair_count\":"
        << distance_evaluated_geometry_pair_count
        << ",\"distance_evaluated_logical_pair_count\":"
        << distance_evaluated_logical_pair_count
        << ",\"mechanism_matches\":"
        << (mechanism_matches ? "true" : "false");
#else
    out << "false"
        << ",\"queried_engine_count\":null"
        << ",\"collision_api_call_count\":null"
        << ",\"collision_enumeration_completion_ordinal\":null"
        << ",\"first_distance_evaluation_ordinal\":null"
        << ",\"active_collision_pair_count\":null"
        << ",\"colliding_geometry_pair_count\":null"
        << ",\"colliding_logical_pair_count\":null"
        << ",\"distance_api_call_count\":null"
        << ",\"distance_evaluated_geometry_pair_count\":null"
        << ",\"distance_evaluated_logical_pair_count\":null"
        << ",\"mechanism_matches\":null";
#endif
    out << "}";
    std::cout << out.str() << '\n';
    return passed ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "perf_contact_grasp_validation_nonregression: "
              << error.what() << '\n';
    return 2;
  }
}
