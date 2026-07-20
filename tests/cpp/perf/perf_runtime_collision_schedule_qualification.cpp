#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/collision/collision_policy_evaluator.hpp>
#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/execution_progress_publication.hpp>
#include <manyforge_core/runtime_collision_lookahead_session.hpp>
#include <manyforge_core/runtime_collision_monitor.hpp>
#include <manyforge_core/types/content_hash.hpp>
#include <manyforge_core/types/scene_edit.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <roboplan_example_models/resources.hpp>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <sched.h>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
#define MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION 0
#endif

#ifndef MANYFORGE_RUNTIME_SCHEDULE_BASELINE
#define MANYFORGE_RUNTIME_SCHEDULE_BASELINE 0
#endif

namespace allocation_probe {

enum class Domain : std::size_t {
  kUnmeasured = 0,
  kImmediateFramework = 1,
  kImmediateEngine = 2,
  kLookaheadEngine = 3,
  kCount = 4,
};

#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
thread_local Domain current_domain = Domain::kUnmeasured;
std::array<std::atomic<std::uint64_t>,
           static_cast<std::size_t>(Domain::kCount)>
    counts{};
#endif

class Scope final {
 public:
  explicit Scope(Domain domain) noexcept {
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
    previous_ = current_domain;
    current_domain = domain;
#else
    (void)domain;
#endif
  }

  ~Scope() {
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
    current_domain = previous_;
#endif
  }

 private:
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  Domain previous_ = Domain::kUnmeasured;
#endif
};

std::uint64_t load(Domain domain) noexcept {
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  return counts[static_cast<std::size_t>(domain)].load(
      std::memory_order_relaxed);
#else
  (void)domain;
  return 0;
#endif
}

void reset() noexcept {
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  for (auto& count : counts) {
    count.store(0, std::memory_order_relaxed);
  }
#endif
}

}  // namespace allocation_probe

#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
namespace {

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

[[gnu::noinline]] void free_bytes(void* pointer) { std::free(pointer); }

void record_allocation() noexcept {
  const auto domain = allocation_probe::current_domain;
  if (domain != allocation_probe::Domain::kUnmeasured) {
    allocation_probe::counts[static_cast<std::size_t>(domain)].fetch_add(
        1, std::memory_order_relaxed);
  }
}

}  // namespace

void* operator new(std::size_t size) {
  record_allocation();
  if (void* pointer = allocate_bytes(size)) {
    return pointer;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return operator new(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  record_allocation();
  return allocate_bytes(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
  return operator new(size, tag);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  record_allocation();
  if (void* pointer = allocate_aligned_bytes(
          size, static_cast<std::size_t>(alignment))) {
    return pointer;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  record_allocation();
  return allocate_aligned_bytes(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
  return operator new(size, alignment, tag);
}

void operator delete(void* pointer) noexcept { free_bytes(pointer); }
void operator delete[](void* pointer) noexcept { free_bytes(pointer); }
void operator delete(void* pointer, std::size_t) noexcept {
  free_bytes(pointer);
}
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
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  free_bytes(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  free_bytes(pointer);
}
void operator delete(void* pointer, std::align_val_t,
                     const std::nothrow_t&) noexcept {
  free_bytes(pointer);
}
void operator delete[](void* pointer, std::align_val_t,
                       const std::nothrow_t&) noexcept {
  free_bytes(pointer);
}
#endif

namespace {

using manyforge::collision::ManyForgeCollisionEngine;
using manyforge::core::AddObjectRequestDTO;
using manyforge::core::AttachObjectRequestDTO;
using manyforge::core::BoxShapeDTO;
using manyforge::core::CollisionCancellationToken;
using manyforge::core::CollisionContactPolicyMode;
#if !MANYFORGE_RUNTIME_SCHEDULE_BASELINE
using manyforge::core::CollisionDistanceRequestMode;
#endif
using manyforge::core::CollisionEngineCapabilityDescriptor;
using manyforge::core::CollisionEngineStatus;
using manyforge::core::CollisionValidationCoordinator;
using manyforge::core::CollisionValidationDecisionBundle;
using manyforge::core::CollisionValidationPassConfig;
using manyforge::core::CollisionPairId;
using manyforge::core::CollisionPolicySnapshot;
using manyforge::core::CollisionQuery;
using manyforge::core::CollisionResult;
using manyforge::core::CollisionShapeDTO;
using manyforge::core::CollisionSurface;
#if !MANYFORGE_RUNTIME_SCHEDULE_BASELINE
using manyforge::core::ContactReleaseEvidenceMode;
#endif
using manyforge::core::CylinderShapeDTO;
using manyforge::core::ExecutionKernelConfig;
using manyforge::core::ExecutionStateUpdateMode;
using manyforge::core::ExecutionProgressAuthorityProfile;
using manyforge::core::ExecutionProgressPublication;
using manyforge::core::ExecutionProgressSourceKind;
using manyforge::core::ICollisionQueryEngine;
using manyforge::core::Pose3DTO;
using manyforge::core::MeshShapeDTO;
using manyforge::core::RobotStateDTO;
using manyforge::core::RuntimeCollisionLookaheadResult;
using manyforge::core::RuntimeCollisionLookaheadResultStatus;
using manyforge::core::RuntimeCollisionLookaheadSession;
using manyforge::core::RuntimeCollisionLookaheadStatus;
using manyforge::core::RuntimeCollisionMonitor;
using manyforge::core::RuntimeCollisionMonitorTimingMetrics;
using manyforge::core::RuntimeCollisionQueryBundlePtr;
using manyforge::core::RuntimeCollisionQueryPrepareStatus;
using manyforge::core::RuntimeCollisionQueryRequest;
using manyforge::core::RuntimeCollisionQueryResultView;
using manyforge::core::RuntimeCollisionQuerySession;
using manyforge::core::RuntimeCollisionStateProjection;
using manyforge::core::RuntimeCollisionTickOutcome;
using manyforge::core::RuntimeCollisionTickStatus;
using manyforge::core::SetCollisionPolicyRequestDTO;
using manyforge::core::SetRobotBasePoseRequestDTO;
using manyforge::core::SupportContactPolicyEntry;
using manyforge::core::SupportContactDTO;
using manyforge::core::TrajectoryDTO;
using manyforge::core::TrajectoryWaypointDTO;
using manyforge::core::Vector3DTO;
using manyforge::roboplan_backend::NativeKernelBundle;
using manyforge::roboplan_backend::create_native_kernel;
using boost::property_tree::ptree;

constexpr std::int64_t kMonitorPeriodNs = 50'000'000;
constexpr std::int64_t kImmediateQueryBudgetNs = 50'000'000;
constexpr std::int64_t kLookaheadCycleBudgetNs = 200'000'000;
constexpr std::int64_t kLookaheadHorizonNs = 2'000'000'000;
constexpr std::int64_t kLookaheadQueryBudgetNs = 75'000'000;
constexpr std::size_t kLookaheadSamples = 32;
constexpr int kLookaheadMinParallelWaypoints = 8;
constexpr std::size_t kTrajectoryWaypointCount = 201;
constexpr double kTrajectoryStepS = 0.02;
constexpr std::string_view kFixtureObjectId =
    "step18_runtime_contact_envelope";
constexpr std::size_t kDistanceDistractorCount = 8;
constexpr double kDistanceDistractorExtentM = 0.10;

enum class Arm {
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
  kCurrentAllActiveDistance,
#else
  kSignedPenetrationSelective,
  kBinaryContact,
  kBoundedPenetration,
#endif
};

const char* arm_name(Arm arm) {
  switch (arm) {
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
    case Arm::kCurrentAllActiveDistance:
      return "current_all_active_distance";
#else
    case Arm::kSignedPenetrationSelective:
      return "signed_penetration_selective";
    case Arm::kBinaryContact:
      return "binary_contact";
    case Arm::kBoundedPenetration:
      return "bounded_penetration";
#endif
  }
  return "unknown";
}

Arm parse_arm(std::string_view value) {
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
  if (value == "current_all_active_distance") {
    return Arm::kCurrentAllActiveDistance;
  }
#else
  if (value == "signed_penetration_selective") {
    return Arm::kSignedPenetrationSelective;
  }
  if (value == "binary_contact") {
    return Arm::kBinaryContact;
  }
  if (value == "bounded_penetration") {
    return Arm::kBoundedPenetration;
  }
  if (value == "current_all_active_distance") {
    throw std::invalid_argument(
        "current_all_active_distance must run against the pinned before "
        "source, not be emulated by the updated binary");
  }
#endif
  throw std::invalid_argument("unknown --arm: " + std::string(value));
}

std::string json_escape(std::string_view value);

struct Options {
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
  Arm arm = Arm::kCurrentAllActiveDistance;
#else
  Arm arm = Arm::kSignedPenetrationSelective;
#endif
  int workers = 2;
  int warmup_ticks = 40;
  int warmup_lookahead_cycles = 5;
  int measured_ticks = 200;
  int measured_lookahead_cycles = 20;
  int max_extra_ticks = 200;
  int campaign_timeout_s = 180;
  double bounded_threshold_m = 2.0;
  std::string run_id = "unspecified";
  std::string process_instance_id = "unknown";
  std::string source_head_commit = "unknown";
  std::string source_head_tree = "unknown";
  bool source_dirty = false;
  std::string source_delta_sha256 = "unknown";
  std::string source_fingerprint_sha256 = "unknown";
  std::string specs_head_commit = "unknown";
  std::string specs_head_tree = "unknown";
  bool specs_dirty = false;
  std::string specs_delta_sha256 = "unknown";
  std::string specs_fingerprint_sha256 = "unknown";
  bool specs_contract_dirty = false;
  std::string specs_contract_delta_sha256 = "unknown";
  std::string specs_contract_fingerprint_sha256 = "unknown";
  std::string image_id = "unknown";
  std::string binary_sha256 = "unknown";
  std::string requested_cpu_affinity = "unbound";
  std::string cpu_governor_set = "unknown";
  std::string cpu_frequency_driver_set = "unknown";
  std::string host_kernel_id = "unknown";
  std::string compiler_flags = "unknown";
  std::string build_type = "unknown";
  std::string source_role = "current_candidate";
  bool checkout_base_clean = false;
  std::string clean_checkout_manifest_sha256;
  std::string qualification_patch_sha256;
  std::string observation_patch_sha256;
  std::string pristine_decision_parity_sha256;
  std::string command_argv_json = "[]";
  std::string runner_command_argv_json = "[]";
  std::string build_commands_argv_json = "[]";
  std::string outer_command_argv_json = "[]";
  std::string runner_environment_json = "{}";
  std::string runner_script_sha256 = "unknown";
  std::string prepare_script_sha256;
  std::string source_identity_script_sha256 = "unknown";
  bool mechanism_replay = false;
  std::filesystem::path mechanism_replay_input;
  std::filesystem::path repo_root = "/workspace";
};

const char* identity_evidence_kind(const Options& options) {
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  return options.source_role == "pristine_baseline_observation"
             ? "baseline_observation_only"
             : "candidate_observation";
#else
  return options.source_role == "pristine_baseline_timing"
             ? "pristine_timing"
             : "candidate_timing";
#endif
}

std::string environment_or(const char* key, std::string fallback) {
  if (const char* value = std::getenv(key); value != nullptr && *value != '\0') {
    return value;
  }
  return fallback;
}

bool environment_bool(const char* key, bool fallback) {
  const auto value = environment_or(key, fallback ? "1" : "0");
  if (value == "1" || value == "true") {
    return true;
  }
  if (value == "0" || value == "false") {
    return false;
  }
  throw std::invalid_argument(std::string(key) + " must be 0/1/false/true");
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
  {
    std::ostringstream command;
    command << '[';
    for (int index = 0; index < argc; ++index) {
      if (index != 0) {
        command << ',';
      }
      command << '"' << json_escape(argv[index]) << '"';
    }
    command << ']';
    options.command_argv_json = command.str();
  }
  options.run_id = environment_or("MANYFORGE_QUAL_RUN_ID", options.run_id);
  options.process_instance_id = environment_or(
      "MANYFORGE_QUAL_PROCESS_INSTANCE_ID", options.process_instance_id);
  options.source_head_commit = environment_or(
      "MANYFORGE_SOURCE_HEAD_COMMIT", options.source_head_commit);
  options.source_head_tree = environment_or(
      "MANYFORGE_SOURCE_HEAD_TREE", options.source_head_tree);
  options.source_dirty =
      environment_bool("MANYFORGE_SOURCE_DIRTY", options.source_dirty);
  options.source_delta_sha256 = environment_or(
      "MANYFORGE_SOURCE_DELTA_SHA256", options.source_delta_sha256);
  options.source_fingerprint_sha256 = environment_or(
      "MANYFORGE_SOURCE_FINGERPRINT_SHA256",
      options.source_fingerprint_sha256);
  options.specs_head_commit = environment_or(
      "MANYFORGE_SPECS_HEAD_COMMIT", options.specs_head_commit);
  options.specs_head_tree = environment_or(
      "MANYFORGE_SPECS_HEAD_TREE", options.specs_head_tree);
  options.specs_dirty =
      environment_bool("MANYFORGE_SPECS_DIRTY", options.specs_dirty);
  options.specs_delta_sha256 = environment_or(
      "MANYFORGE_SPECS_DELTA_SHA256", options.specs_delta_sha256);
  options.specs_fingerprint_sha256 = environment_or(
      "MANYFORGE_SPECS_FINGERPRINT_SHA256",
      options.specs_fingerprint_sha256);
  options.specs_contract_dirty = environment_bool(
      "MANYFORGE_SPECS_CONTRACT_DIRTY", options.specs_contract_dirty);
  options.specs_contract_delta_sha256 = environment_or(
      "MANYFORGE_SPECS_CONTRACT_DELTA_SHA256",
      options.specs_contract_delta_sha256);
  options.specs_contract_fingerprint_sha256 = environment_or(
      "MANYFORGE_SPECS_CONTRACT_FINGERPRINT_SHA256",
      options.specs_contract_fingerprint_sha256);
  options.image_id =
      environment_or("MANYFORGE_QUAL_IMAGE_ID", options.image_id);
  options.binary_sha256 = environment_or(
      "MANYFORGE_QUAL_BINARY_SHA256", options.binary_sha256);
  options.requested_cpu_affinity = environment_or(
      "MANYFORGE_RUNTIME_SCHEDULE_CPUSET", options.requested_cpu_affinity);
  options.cpu_governor_set = environment_or(
      "MANYFORGE_CPU_GOVERNOR_SET", options.cpu_governor_set);
  options.cpu_frequency_driver_set = environment_or(
      "MANYFORGE_CPU_FREQUENCY_DRIVER_SET",
      options.cpu_frequency_driver_set);
  options.host_kernel_id =
      environment_or("MANYFORGE_HOST_KERNEL_ID", options.host_kernel_id);
  options.compiler_flags = environment_or(
      "MANYFORGE_MATCHED_COMPILER_FLAGS", options.compiler_flags);
  options.build_type =
      environment_or("MANYFORGE_MATCHED_BUILD_TYPE", options.build_type);
  options.source_role =
      environment_or("MANYFORGE_SOURCE_ROLE", options.source_role);
  options.checkout_base_clean =
      environment_bool("MANYFORGE_CHECKOUT_BASE_CLEAN", false);
  options.clean_checkout_manifest_sha256 = environment_or(
      "MANYFORGE_CLEAN_CHECKOUT_MANIFEST_SHA256", "");
  options.qualification_patch_sha256 = environment_or(
      "MANYFORGE_QUALIFICATION_PATCH_SHA256", "");
  options.observation_patch_sha256 =
      environment_or("MANYFORGE_OBSERVATION_PATCH_SHA256", "");
  options.pristine_decision_parity_sha256 = environment_or(
      "MANYFORGE_PRISTINE_DECISION_PARITY_SHA256", "");
  options.runner_command_argv_json = environment_or(
      "MANYFORGE_QUAL_RUNNER_COMMAND_ARGV_JSON",
      options.runner_command_argv_json);
  options.build_commands_argv_json = environment_or(
      "MANYFORGE_QUAL_BUILD_COMMANDS_ARGV_JSON",
      options.build_commands_argv_json);
  options.outer_command_argv_json = environment_or(
      "MANYFORGE_QUAL_OUTER_COMMAND_ARGV_JSON",
      options.outer_command_argv_json);
  options.runner_environment_json = environment_or(
      "MANYFORGE_QUAL_RUNNER_ENVIRONMENT_JSON",
      options.runner_environment_json);
  options.runner_script_sha256 = environment_or(
      "MANYFORGE_QUAL_RUNNER_SCRIPT_SHA256",
      options.runner_script_sha256);
  options.prepare_script_sha256 = environment_or(
      "MANYFORGE_QUAL_PREPARE_SCRIPT_SHA256", "");
  options.source_identity_script_sha256 = environment_or(
      "MANYFORGE_SOURCE_IDENTITY_SCRIPT_SHA256",
      options.source_identity_script_sha256);
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    auto require_value = [&]() -> std::string {
      if (index + 1 >= argc) {
        throw std::invalid_argument(option + " requires a value");
      }
      return argv[++index];
    };
    if (option == "--arm") {
      options.arm = parse_arm(require_value());
    } else if (option == "--workers") {
      options.workers = parse_positive_int(require_value(), "--workers", 64);
    } else if (option == "--warmup-ticks") {
      options.warmup_ticks =
          parse_positive_int(require_value(), "--warmup-ticks");
    } else if (option == "--warmup-lookahead-cycles") {
      options.warmup_lookahead_cycles = parse_positive_int(
          require_value(), "--warmup-lookahead-cycles");
    } else if (option == "--measured-ticks") {
      options.measured_ticks =
          parse_positive_int(require_value(), "--measured-ticks");
    } else if (option == "--measured-lookahead-cycles") {
      options.measured_lookahead_cycles = parse_positive_int(
          require_value(), "--measured-lookahead-cycles");
    } else if (option == "--max-extra-ticks") {
      options.max_extra_ticks =
          parse_positive_int(require_value(), "--max-extra-ticks");
    } else if (option == "--campaign-timeout-s") {
      options.campaign_timeout_s = parse_positive_int(
          require_value(), "--campaign-timeout-s", 3600);
    } else if (option == "--bounded-threshold-m") {
      const auto value = require_value();
      std::size_t consumed = 0;
      options.bounded_threshold_m = std::stod(value, &consumed);
      if (consumed != value.size() ||
          !std::isfinite(options.bounded_threshold_m) ||
          options.bounded_threshold_m < 0.0) {
        throw std::invalid_argument(
            "--bounded-threshold-m must be finite and non-negative");
      }
    } else if (option == "--run-id") {
      options.run_id = require_value();
    } else if (option == "--image-id") {
      options.image_id = require_value();
    } else if (option == "--binary-sha256") {
      options.binary_sha256 = require_value();
    } else if (option == "--cpu-affinity") {
      options.requested_cpu_affinity = require_value();
    } else if (option == "--measurement-kind") {
      const auto requested = require_value();
      const std::string compiled = MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
                                       ? "observation"
                                       : "timing";
      if (requested != compiled) {
        throw std::invalid_argument(
            "requested measurement kind does not match the selected binary");
      }
    } else if (option == "--mechanism-replay-input") {
      options.mechanism_replay = true;
      options.mechanism_replay_input = require_value();
    } else if (option == "--repo-root") {
      options.repo_root = require_value();
    } else if (option == "--help") {
      std::cout
          << "usage: perf_runtime_collision_schedule_{timing,observation} "
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
             "--arm current_all_active_distance [options]\n";
#else
             "--arm {signed_penetration_selective,binary_contact,"
             "bounded_penetration} [options]\n";
#endif
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + option);
    }
  }
  if (options.measured_lookahead_cycles >
      options.measured_ticks + options.max_extra_ticks) {
    throw std::invalid_argument(
        "lookahead cycle target exceeds the maximum scheduled tick count");
  }
  if (options.mechanism_replay && options.mechanism_replay_input.empty()) {
    throw std::invalid_argument(
        "--mechanism-replay-input requires a non-empty path");
  }
  return options;
}

std::int64_t clock_ns(clockid_t id) noexcept {
  timespec value{};
  if (::clock_gettime(id, &value) != 0) {
    return -1;
  }
  return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000LL +
         static_cast<std::int64_t>(value.tv_nsec);
}

std::int64_t wall_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string effective_cpu_affinity() {
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  if (::sched_getaffinity(0, sizeof(cpus), &cpus) != 0) {
    return "unavailable";
  }
  std::ostringstream out;
  out << std::setprecision(17);
  bool first = true;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &cpus)) {
      continue;
    }
    if (!first) {
      out << ',';
    }
    first = false;
    out << cpu;
  }
  return out.str();
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

const char* json_bool(bool value) { return value ? "true" : "false"; }

void append_nullable_json_string(std::ostringstream& out,
                                 const std::string& value) {
  if (value.empty()) {
    out << "null";
  } else {
    out << '\"' << json_escape(value) << '\"';
  }
}

void append_source_identity(std::ostringstream& out,
                            const std::string& head_commit,
                            const std::string& head_tree, bool dirty,
                            const std::string& delta_sha256,
                            const std::string& fingerprint_sha256) {
  out << "{\"schema_version\":\"manyforge.source_identity.v1\""
      << ",\"head_commit\":\"" << json_escape(head_commit)
      << "\",\"head_tree\":\"" << json_escape(head_tree)
      << "\",\"dirty\":" << json_bool(dirty)
      << ",\"delta_sha256\":\"" << json_escape(delta_sha256)
      << "\",\"fingerprint_sha256\":\""
      << json_escape(fingerprint_sha256) << "\"}";
}

void append_specs_contract_identity(
    std::ostringstream& out, const std::string& head_commit,
    const std::string& head_tree, bool contract_dirty,
    const std::string& contract_delta_sha256,
    const std::string& contract_fingerprint_sha256) {
  out << "{\"schema_version\":\"manyforge.specs_contract_identity.v1\""
      << ",\"head_commit\":\"" << json_escape(head_commit)
      << "\",\"head_tree\":\"" << json_escape(head_tree)
      << "\",\"excluded_paths\":[\"docs/implementation/"
         "contact-release-evidence-modes-step18-evidence-2026-07-19.md\"]"
      << ",\"contract_dirty\":" << json_bool(contract_dirty)
      << ",\"contract_delta_sha256\":\""
      << json_escape(contract_delta_sha256)
      << "\",\"contract_fingerprint_sha256\":\""
      << json_escape(contract_fingerprint_sha256) << "\"}";
}

CollisionPairId canonical_pair(std::string body_a, std::string body_b) {
  if (body_b < body_a) {
    std::swap(body_a, body_b);
  }
  return {std::move(body_a), std::move(body_b)};
}

Pose3DTO pose(double x, double y, double z) {
  Pose3DTO value;
  value.position_m = Vector3DTO{x, y, z};
  value.orientation.w = 1.0;
  return value;
}

RobotStateDTO observed_state(const NativeKernelBundle& native) {
  auto state = native.observed_state_provider->current_state(
      native.clock->now());
  if (!state.ok()) {
    throw std::runtime_error("observed state unavailable: " +
                             state.error().message);
  }
  return std::move(state.value());
}

std::shared_ptr<ManyForgeCollisionEngine> make_probe_engine() {
  const auto models_dir = roboplan::example_models::get_package_models_dir();
  const auto package_share =
      roboplan::example_models::get_package_share_dir();
  return std::make_shared<ManyForgeCollisionEngine>(
      "ur5_gripper", models_dir / "ur_robot_model/ur5_gripper.urdf",
      models_dir / "ur_robot_model/ur5_gripper.srdf",
      std::vector<std::filesystem::path>{package_share}, "arm");
}

struct FixtureInputs {
  RobotStateDTO state;
  RuntimeCollisionQueryBundlePtr bundle;
  std::shared_ptr<const std::vector<CollisionPairId>> allowed_pairs;
  double maximum_observed_penetration_m = 0.0;
  bool bounded_any_allowed_pair_late_contact_sequence_accepted = false;
  bool bounded_over_cap_transition_rejected = false;
  int parity_probe_engine_status = 0;
  int parity_probe_decision = 0;
  std::vector<CollisionPairId> parity_probe_normalized_pairs;
  std::string parity_probe_normalized_pair_output_fingerprint;
  std::string geometry_fingerprint;
  std::string pair_fingerprint;
  std::string policy_fingerprint;
};

std::string pair_fingerprint(
    const std::vector<CollisionPairId>& pairs) {
  manyforge::core::ContentHasher hasher;
  hasher.mix("manyforge.step18.runtime-contact-pairs.v1");
  for (const auto& pair : pairs) {
    hasher.mix(pair.body_a).mix(pair.body_b);
  }
  return hasher.finalize();
}

CollisionPolicySnapshot make_policy(
    Arm arm, const std::vector<CollisionPairId>& pairs,
    double bounded_threshold_m) {
  CollisionPolicySnapshot policy;
  policy.policy_id = std::string("step18_runtime_") + arm_name(arm);
  policy.revision.revision_id = 1;
  manyforge::core::ContentHasher hasher;
  hasher.mix("manyforge.step18.runtime-policy.v1").mix(arm_name(arm));
  for (const auto& pair : pairs) {
    SupportContactPolicyEntry entry;
    entry.pair = pair;
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
    (void)arm;
    entry.mode = CollisionContactPolicyMode::kContactRelease;
    entry.enforce_max_start_penetration = true;
    entry.max_start_penetration_m = bounded_threshold_m;
    entry.release_tolerance_m = 1e-5;
    entry.min_release_progress_m = 0.0;
    entry.require_separation_by_end = true;
    entry.require_strictly_decreasing_penetration = true;
    entry.allow_recontact = false;
    hasher.mix(entry.pair.body_a)
        .mix(entry.pair.body_b)
        .mix(static_cast<std::uint64_t>(entry.mode))
        .mix(static_cast<std::uint64_t>(entry.enforce_max_start_penetration))
        .mix(entry.max_start_penetration_m)
        .mix(entry.release_tolerance_m)
        .mix(entry.min_release_progress_m)
        .mix(static_cast<std::uint64_t>(entry.require_separation_by_end))
        .mix(static_cast<std::uint64_t>(
            entry.require_strictly_decreasing_penetration))
        .mix(static_cast<std::uint64_t>(entry.allow_recontact));
#else
    if (arm == Arm::kBinaryContact) {
      entry.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
      entry.enforce_max_start_penetration = false;
      entry.max_start_penetration_m = 0.0;
      entry.release_tolerance_m = 0.0;
      entry.min_release_progress_m = 0.0;
      entry.max_contact_penetration_m = 0.0;
      entry.require_strictly_decreasing_penetration = false;
    } else if (arm == Arm::kBoundedPenetration) {
      entry.mode = CollisionContactPolicyMode::kBoundedPenetration;
      entry.enforce_max_start_penetration = false;
      entry.max_start_penetration_m = 0.0;
      entry.release_tolerance_m = 0.0;
      entry.min_release_progress_m = 0.0;
      entry.max_contact_penetration_m = bounded_threshold_m;
      entry.require_separation_by_end = false;
      entry.require_strictly_decreasing_penetration = false;
      entry.allow_recontact = false;
    } else {
      entry.max_start_penetration_m = bounded_threshold_m;
      entry.release_tolerance_m = 1e-5;
      entry.min_release_progress_m = 0.0;
      entry.require_separation_by_end = true;
      entry.require_strictly_decreasing_penetration = true;
    }
    hasher.mix(entry.pair.body_a)
        .mix(entry.pair.body_b)
        .mix(static_cast<std::uint64_t>(entry.mode))
        .mix(static_cast<std::uint64_t>(entry.release_evidence_mode))
        .mix(static_cast<std::uint64_t>(entry.enforce_max_start_penetration))
        .mix(entry.max_start_penetration_m)
        .mix(entry.max_contact_penetration_m)
        .mix(entry.release_tolerance_m)
        .mix(entry.min_release_progress_m)
        .mix(static_cast<std::uint64_t>(entry.require_separation_by_end))
        .mix(static_cast<std::uint64_t>(
            entry.require_strictly_decreasing_penetration))
        .mix(static_cast<std::uint64_t>(entry.allow_recontact));
#endif
    policy.support_contacts.push_back(std::move(entry));
  }
  policy.revision.content_hash = hasher.finalize();
  return policy;
}

template <typename T>
std::vector<T> replay_array_values(const ptree& tree) {
  std::vector<T> values;
  values.reserve(tree.size());
  for (const auto& [key, value] : tree) {
    (void)key;
    values.push_back(value.template get_value<T>());
  }
  return values;
}

Pose3DTO replay_pose(const ptree& value) {
  const auto position =
      replay_array_values<double>(value.get_child("position_m"));
  const auto orientation =
      replay_array_values<double>(value.get_child("orientation_xyzw"));
  if (position.size() != 3 || orientation.size() != 4) {
    throw std::invalid_argument("mechanism replay pose has invalid dimensions");
  }
  Pose3DTO result;
  result.position_m = Vector3DTO{position[0], position[1], position[2]};
  result.orientation.x = orientation[0];
  result.orientation.y = orientation[1];
  result.orientation.z = orientation[2];
  result.orientation.w = orientation[3];
  return result;
}

CollisionPairId replay_pair(const ptree& value) {
  return canonical_pair(value.get<std::string>("body_a"),
                        value.get<std::string>("body_b"));
}

void require_replay_scene_edit(const manyforge::core::SceneEditResultDTO& result,
                               std::string_view action) {
  if (!result.success) {
    throw std::runtime_error(
        result.error ? result.error->message
                     : "mechanism replay scene edit failed: " +
                           std::string(action));
  }
}

void configure_replay_contact(SupportContactPolicyEntry& entry, Arm arm,
                              const ptree& source,
                              double bounded_threshold_m) {
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
  (void)arm;
  (void)bounded_threshold_m;
  if (source.get<std::string>("mode") != "contact_release") {
    throw std::invalid_argument(
        "baseline mechanism replay requires captured contact_release policy");
  }
  entry.mode = CollisionContactPolicyMode::kContactRelease;
  entry.enforce_max_start_penetration =
      source.get<bool>("enforce_max_start_penetration");
  entry.max_start_penetration_m =
      source.get<double>("max_start_penetration_m");
  entry.release_tolerance_m = source.get<double>("release_tolerance_m");
  entry.min_release_progress_m =
      source.get<double>("min_release_progress_m");
  entry.min_separation_clearance_m =
      source.get<double>("min_separation_clearance_m", 0.0);
  entry.require_separation_by_end =
      source.get<bool>("require_separation_by_end");
  entry.require_strictly_decreasing_penetration =
      source.get<bool>("require_strictly_decreasing_penetration");
  entry.allow_recontact = source.get<bool>("allow_recontact");
#else
  if (arm == Arm::kBinaryContact) {
    entry.mode = CollisionContactPolicyMode::kContactRelease;
    entry.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
    entry.enforce_max_start_penetration = false;
    entry.max_start_penetration_m = 0.0;
    entry.release_tolerance_m = 0.0;
    entry.min_release_progress_m = 0.0;
    entry.max_contact_penetration_m = 0.0;
    entry.require_separation_by_end = true;
    entry.require_strictly_decreasing_penetration = false;
    entry.allow_recontact = false;
  } else if (arm == Arm::kBoundedPenetration) {
    entry.mode = CollisionContactPolicyMode::kBoundedPenetration;
    entry.release_evidence_mode = ContactReleaseEvidenceMode::kSignedPenetration;
    entry.enforce_max_start_penetration = false;
    entry.max_start_penetration_m = 0.0;
    entry.release_tolerance_m = 0.0;
    entry.min_release_progress_m = 0.0;
    entry.max_contact_penetration_m = bounded_threshold_m;
    entry.require_separation_by_end = false;
    entry.require_strictly_decreasing_penetration = false;
    entry.allow_recontact = false;
  } else {
    if (source.get<std::string>("mode") != "contact_release") {
      throw std::invalid_argument(
          "signed mechanism replay requires captured contact_release policy");
    }
    entry.mode = CollisionContactPolicyMode::kContactRelease;
    entry.release_evidence_mode = ContactReleaseEvidenceMode::kSignedPenetration;
    entry.enforce_max_start_penetration =
        source.get<bool>("enforce_max_start_penetration");
    entry.max_start_penetration_m =
        source.get<double>("max_start_penetration_m");
    entry.release_tolerance_m = source.get<double>("release_tolerance_m");
    entry.min_release_progress_m =
        source.get<double>("min_release_progress_m");
    entry.max_contact_penetration_m = 0.0;
    entry.require_separation_by_end =
        source.get<bool>("require_separation_by_end");
    entry.require_strictly_decreasing_penetration =
        source.get<bool>("require_strictly_decreasing_penetration");
    entry.allow_recontact = source.get<bool>("allow_recontact");
  }
#endif
}

CollisionPolicySnapshot replay_policy(const ptree& capture,
                                      const Options& options) {
  const auto& source =
      capture.get_child("scene.authority.configured_collision_policy");
  CollisionPolicySnapshot policy;
  policy.policy_id = "step18_isaac_live_replay_" +
                     std::string(arm_name(options.arm));
  policy.revision.revision_id =
      source.get<std::uint64_t>("revision.revision_id");
  manyforge::core::ContentHasher policy_hash;
  policy_hash.mix("manyforge.step18.isaac-live-replay-policy.v1")
      .mix(arm_name(options.arm));
  for (const auto& [key, value] :
       source.get_child("allowed_static_static_pairs")) {
    (void)key;
    policy.allowed_static_static_pairs.push_back(replay_pair(value));
  }
  for (const auto& [key, value] :
       source.get_child("allowed_touch_link_pairs")) {
    (void)key;
    policy.allowed_touch_link_pairs.push_back(replay_pair(value));
  }
  for (const auto& [key, value] : source.get_child("support_contacts")) {
    (void)key;
    SupportContactPolicyEntry entry;
    entry.pair = replay_pair(value.get_child("pair"));
    configure_replay_contact(entry, options.arm, value,
                             options.bounded_threshold_m);
    policy_hash.mix(entry.pair.body_a).mix(entry.pair.body_b);
    policy.support_contacts.push_back(std::move(entry));
  }
  policy.require_full_diagnostics = source.get<bool>("require_full_diagnostics");
  policy.revision.content_hash = policy_hash.finalize();
  return policy;
}

SupportContactDTO replay_authored_contact(const ptree& source, Arm arm,
                                          double bounded_threshold_m) {
  SupportContactDTO contact;
  contact.support_object_id =
      source.get<std::string>("support_object_id");
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
  (void)arm;
  (void)bounded_threshold_m;
  contact.mode = CollisionContactPolicyMode::kContactRelease;
  contact.enforce_max_start_penetration =
      source.get<bool>("enforce_max_start_penetration", true);
  contact.max_start_penetration_m =
      source.get<double>("max_start_penetration_m", 0.0);
  contact.release_tolerance_m =
      source.get<double>("release_tolerance_m", 1e-5);
  contact.min_release_progress_m =
      source.get<double>("min_release_progress_m", 0.0);
  contact.min_separation_clearance_m =
      source.get<double>("min_separation_clearance_m", 0.0);
  contact.require_separation_by_end =
      source.get<bool>("require_separation_by_end", true);
  contact.require_strictly_decreasing_penetration =
      source.get<bool>("require_strictly_decreasing_penetration", true);
  contact.allow_recontact = source.get<bool>("allow_recontact", false);
#else
  SupportContactPolicyEntry resolved;
  configure_replay_contact(resolved, arm, source, bounded_threshold_m);
  contact.mode = resolved.mode;
  contact.release_evidence_mode = resolved.release_evidence_mode;
  contact.enforce_max_start_penetration =
      resolved.enforce_max_start_penetration;
  contact.max_start_penetration_m = resolved.max_start_penetration_m;
  contact.release_tolerance_m = resolved.release_tolerance_m;
  contact.min_release_progress_m = resolved.min_release_progress_m;
  contact.max_contact_penetration_m = resolved.max_contact_penetration_m;
  contact.require_separation_by_end = resolved.require_separation_by_end;
  contact.require_strictly_decreasing_penetration =
      resolved.require_strictly_decreasing_penetration;
  contact.allow_recontact = resolved.allow_recontact;
#endif
  return contact;
}

void seed_replay_projection(NativeKernelBundle& native,
                            const ptree& capture) {
  for (const auto& [key, group] : capture.get_child(
           "scene.robot.collision_projection_seed.groups")) {
    (void)key;
    const auto group_id = group.get<std::string>("group_id");
    const auto names = replay_array_values<std::string>(
        group.get_child("joint_names"));
    const auto positions =
        replay_array_values<double>(group.get_child("positions_rad"));
    if (names.empty() || names.size() != positions.size()) {
      throw std::invalid_argument(
          "mechanism replay projection seed has invalid shape");
    }
    const auto applied = native.execution_state_sink->apply_group_joint_positions(
        group_id, positions, ExecutionStateUpdateMode::kBlocking);
    if (!applied.applied()) {
      throw std::runtime_error(
          "mechanism replay could not apply projection seed group " + group_id);
    }
  }
}

void configure_replay_world(NativeKernelBundle& native, const ptree& capture,
                            const Options& options) {
  SetCollisionPolicyRequestDTO policy_request;
  policy_request.policy = replay_policy(capture, options);
  require_replay_scene_edit(
      native.scene_authority->set_collision_policy(policy_request),
      "collision policy");

  SetRobotBasePoseRequestDTO base;
  base.robot_id = capture.get<std::string>("scene.robot.robot_id");
  base.pose_in_world = replay_pose(
      capture.get_child("scene.robot.pose_in_world"));
  require_replay_scene_edit(native.scene_authority->set_robot_base_pose(base),
                            "robot base pose");

  for (const auto& [key, object] : capture.get_child("scene.objects")) {
    (void)key;
    AddObjectRequestDTO request;
    request.object_id = object.get<std::string>("object_id");
    request.frame_id = "universe";
    request.pose = replay_pose(object.get_child("pose_in_world"));
    const auto shape_type = object.get<std::string>("shape.type");
    if (shape_type == "mesh") {
      const auto scale = replay_array_values<double>(
          object.get_child("shape.mesh_scale"));
      if (scale.size() != 3) {
        throw std::invalid_argument(
            "mechanism replay mesh scale has invalid dimensions");
      }
      auto resource =
          std::filesystem::path(object.get<std::string>("shape.resource_uri"));
      if (!resource.is_absolute()) {
        resource = options.repo_root / resource;
      }
      request.shape = CollisionShapeDTO{MeshShapeDTO{
          resource.string(), Vector3DTO{scale[0], scale[1], scale[2]}}};
    } else if (shape_type == "box") {
      const auto dimensions = replay_array_values<double>(
          object.get_child("shape.dimensions_m"));
      if (dimensions.size() != 3) {
        throw std::invalid_argument(
            "mechanism replay box has invalid dimensions");
      }
      request.shape = CollisionShapeDTO{BoxShapeDTO{Vector3DTO{
          dimensions[0], dimensions[1], dimensions[2]}}};
    } else {
      throw std::invalid_argument("unsupported mechanism replay shape: " +
                                  shape_type);
    }
    if (const auto contacts = object.get_child_optional("support_contacts")) {
      for (const auto& [contact_key, contact] : *contacts) {
        (void)contact_key;
        request.support_contacts.push_back(replay_authored_contact(
            contact, options.arm, options.bounded_threshold_m));
      }
    }
    require_replay_scene_edit(native.scene_authority->add_object(request),
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
    request.touch_links = replay_array_values<std::string>(
        object.get_child("touch_links"));
    require_replay_scene_edit(native.scene_authority->attach_object(request),
                              "object attach");
  }

}

TrajectoryDTO replay_trajectory(const ptree& capture) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = replay_array_values<std::string>(
      capture.get_child("planning_result.trajectory.joint_names"));
  for (const auto& [key, value] :
       capture.get_child("planning_result.trajectory.waypoints")) {
    (void)key;
    TrajectoryWaypointDTO waypoint;
    waypoint.time_from_start_s = value.get<double>("time_from_start_s");
    waypoint.positions_rad =
        replay_array_values<double>(value.get_child("positions_rad"));
    if (const auto velocities = value.get_child_optional("velocities_rad_s")) {
      waypoint.velocities_rad_s = replay_array_values<double>(*velocities);
    }
    trajectory.waypoints.push_back(std::move(waypoint));
  }
  if (trajectory.joint_names.empty() || trajectory.waypoints.size() < 2) {
    throw std::invalid_argument("mechanism replay trajectory is incomplete");
  }
  return trajectory;
}

RobotStateDTO replay_start_state(const ptree& capture) {
  RobotStateDTO state;
  state.joint_names = replay_array_values<std::string>(
      capture.get_child("plan_request.start_state.joint_names"));
  state.positions_rad = replay_array_values<double>(
      capture.get_child("plan_request.start_state.positions_rad"));
  state.velocities_rad_s = replay_array_values<double>(
      capture.get_child("plan_request.start_state.velocities_rad_s"));
  if (state.joint_names.size() != state.positions_rad.size()) {
    throw std::invalid_argument("mechanism replay start state is malformed");
  }
  return state;
}

FixtureInputs configure_fixture(NativeKernelBundle& native, const Options& options) {
  AddObjectRequestDTO object;
  object.object_id = std::string(kFixtureObjectId);
  object.shape = CollisionShapeDTO{CylinderShapeDTO{1.0, 3.0}};
  object.pose = pose(0.0, 0.0, 0.75);
  object.frame_id = "universe";
  const auto added = native.scene_authority->add_object(object);
  if (!added.success) {
    throw std::runtime_error(
        added.error ? added.error->message : "failed to add contact envelope");
  }
  for (std::size_t index = 0; index < kDistanceDistractorCount; ++index) {
    AddObjectRequestDTO distractor;
    distractor.object_id = "step18_runtime_distance_distractor_" +
                           std::to_string(index);
    distractor.shape = CollisionShapeDTO{BoxShapeDTO{Vector3DTO{
        kDistanceDistractorExtentM, kDistanceDistractorExtentM,
        kDistanceDistractorExtentM}}};
    distractor.pose =
        pose(5.0 + static_cast<double>(index) * 0.25, 5.0, 5.0);
    distractor.frame_id = "universe";
    const auto distractor_added =
        native.scene_authority->add_object(distractor);
    if (!distractor_added.success) {
      throw std::runtime_error(
          distractor_added.error ? distractor_added.error->message
                                 : "failed to add distance distractor");
    }
  }
  const auto world = native.scene_authority->snapshot();
  if (!world) {
    throw std::runtime_error("scene authority published no contact fixture");
  }

  FixtureInputs inputs;
  inputs.state = observed_state(native);
  auto probe = make_probe_engine();
  CollisionQuery query;
  query.query_id = "step18_runtime_contact_discovery";
  query.surface = CollisionSurface::kDiagnosticCompare;
  query.robot_state = inputs.state;
  query.collision_world_revision = world->revision;
  query.required_capabilities = {"binary_collision", "body_pair_only",
                                 "penetration_depth"};
#if !MANYFORGE_RUNTIME_SCHEDULE_BASELINE
  query.distance_request_mode = CollisionDistanceRequestMode::kAllCollidingPairs;
#endif
  query.timeout_budget_ns = 250'000'000;
  CollisionCancellationToken cancellation;
  CollisionPolicySnapshot discovery_policy;
  const auto discovery =
      probe->check(*world, query, discovery_policy, cancellation);
  if (discovery.engine_status != CollisionEngineStatus::kOk) {
    throw std::runtime_error("contact-pair discovery failed closed");
  }

  const std::string object_body =
      "object/" + std::string(kFixtureObjectId) + "/shape/0";
  std::vector<CollisionPairId> pairs;
  for (const auto& pair : discovery.normalized_pairs) {
    if (pair.body_a == object_body || pair.body_b == object_body) {
      pairs.push_back(canonical_pair(pair.body_a, pair.body_b));
    }
  }
  std::sort(pairs.begin(), pairs.end());
  pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
  if (pairs.empty()) {
    throw std::runtime_error(
        "contact envelope produced no object/robot collision pairs");
  }
  for (const auto& clearance : discovery.pair_clearances) {
    const auto pair = canonical_pair(clearance.pair.body_a,
                                     clearance.pair.body_b);
    if (std::binary_search(pairs.begin(), pairs.end(), pair) &&
        std::isfinite(clearance.signed_clearance_m) &&
        clearance.signed_clearance_m < 0.0) {
      inputs.maximum_observed_penetration_m =
          std::max(inputs.maximum_observed_penetration_m,
                   -clearance.signed_clearance_m);
    }
  }
  if (inputs.maximum_observed_penetration_m <= 0.0) {
    throw std::runtime_error(
        "contact discovery returned no finite penetrating metric evidence");
  }
#if !MANYFORGE_RUNTIME_SCHEDULE_BASELINE
  {
    SupportContactPolicyEntry bounded;
    bounded.mode = CollisionContactPolicyMode::kBoundedPenetration;
    bounded.max_contact_penetration_m = options.bounded_threshold_m;
    manyforge::core::ContactReleaseEvaluationState state;
    const std::array<manyforge::core::ContactReleaseSampleEvidence, 5>
        late_contact_sequence{
            manyforge::core::ContactReleaseSampleEvidence{false,
                                                          std::nullopt},
            manyforge::core::ContactReleaseSampleEvidence{
                true, -inputs.maximum_observed_penetration_m},
            manyforge::core::ContactReleaseSampleEvidence{false,
                                                          std::nullopt},
            manyforge::core::ContactReleaseSampleEvidence{
                true, -inputs.maximum_observed_penetration_m},
            manyforge::core::ContactReleaseSampleEvidence{
                true, -inputs.maximum_observed_penetration_m},
        };
    // Bounded penetration applies to every configured allowed pair at every
    // sample.  A pair need not belong to the initial contact set: it may first
    // appear later, disappear, reappear, and remain in contact at trajectory
    // end, provided every observed depth stays within the cap.
    inputs.bounded_any_allowed_pair_late_contact_sequence_accepted = true;
    for (const auto& sample : late_contact_sequence) {
      if (!manyforge::core::advance_contact_release_sample(bounded, sample,
                                                           state)
               .accepted()) {
        inputs.bounded_any_allowed_pair_late_contact_sequence_accepted = false;
        break;
      }
    }
    if (!inputs.bounded_any_allowed_pair_late_contact_sequence_accepted ||
        !state.in_contact) {
      throw std::runtime_error(
          "bounded clear-to-late-contact adversarial sequence was rejected");
    }
  }
  {
    SupportContactPolicyEntry over_cap;
    over_cap.mode = CollisionContactPolicyMode::kBoundedPenetration;
    over_cap.max_contact_penetration_m =
        inputs.maximum_observed_penetration_m * 0.5;
    manyforge::core::ContactReleaseEvaluationState state;
    const auto transition = manyforge::core::advance_contact_release_sample(
        over_cap,
        manyforge::core::ContactReleaseSampleEvidence{
            true, -inputs.maximum_observed_penetration_m},
        state);
    inputs.bounded_over_cap_transition_rejected =
        transition.failure == manyforge::core::
                                  ContactReleaseTransitionFailure::
                                      kContactPenetrationExceedsPolicy;
    if (!inputs.bounded_over_cap_transition_rejected) {
      throw std::runtime_error(
          "bounded over-cap adversarial transition was not rejected");
    }
  }
  if (inputs.maximum_observed_penetration_m >
      options.bounded_threshold_m) {
    throw std::runtime_error(
        "declared threshold does not admit the expected fixture contacts");
  }
#endif

  auto policy = make_policy(options.arm, pairs,
                            options.bounded_threshold_m);
  inputs.policy_fingerprint = policy.revision.content_hash;
  SetCollisionPolicyRequestDTO request;
  request.policy = std::move(policy);
  const auto applied = native.scene_authority->set_collision_policy(request);
  if (!applied.success) {
    throw std::runtime_error(
        applied.error ? applied.error->message : "failed to apply policy");
  }
  inputs.bundle =
      native.scene_authority->published_runtime_collision_query_bundle();
  if (!inputs.bundle || !inputs.bundle->world || !inputs.bundle->policy ||
      !inputs.bundle->policy_index) {
    throw std::runtime_error("runtime bundle was not published coherently");
  }
  if (inputs.bundle->policy->support_contacts.size() != pairs.size()) {
    throw std::runtime_error("published policy lost configured contact pairs");
  }

  // Dedicated, unmeasured output-parity query. It is outside every cold/warm
  // timing window and uses the same empty policy for every arm so the
  // canonical physical collision output is not filtered by the arm-specific
  // allowed-contact decision. This binds baseline/current output rather than
  // merely comparing the authored allowed-pair input.
  CollisionQuery parity_query;
  parity_query.query_id = "step18_runtime_output_parity";
  parity_query.surface = CollisionSurface::kDiagnosticCompare;
  parity_query.robot_state = inputs.state;
  parity_query.collision_world_revision = world->revision;
  parity_query.required_capabilities = {"binary_collision",
                                        "body_pair_only"};
  parity_query.timeout_budget_ns = 250'000'000;
  CollisionCancellationToken parity_cancellation;
  const auto parity_result = probe->check(
      *world, parity_query, discovery_policy, parity_cancellation);
  if (parity_result.engine_status != CollisionEngineStatus::kOk ||
      !parity_result.in_collision) {
    throw std::runtime_error("unmeasured output-parity probe failed");
  }
  inputs.parity_probe_engine_status =
      static_cast<int>(parity_result.engine_status);
  inputs.parity_probe_decision = static_cast<int>(parity_result.decision);
  inputs.parity_probe_normalized_pairs = parity_result.normalized_pairs;
  std::sort(inputs.parity_probe_normalized_pairs.begin(),
            inputs.parity_probe_normalized_pairs.end());
  inputs.parity_probe_normalized_pairs.erase(
      std::unique(inputs.parity_probe_normalized_pairs.begin(),
                  inputs.parity_probe_normalized_pairs.end()),
      inputs.parity_probe_normalized_pairs.end());
  manyforge::core::ContentHasher parity_output;
  parity_output.mix("manyforge.step18.runtime-parity-output.v1");
  for (const auto& output_pair : inputs.parity_probe_normalized_pairs) {
    parity_output.mix(output_pair.body_a).mix(output_pair.body_b);
  }
  inputs.parity_probe_normalized_pair_output_fingerprint =
      parity_output.finalize();
  inputs.allowed_pairs =
      std::make_shared<const std::vector<CollisionPairId>>(std::move(pairs));
  inputs.pair_fingerprint = pair_fingerprint(*inputs.allowed_pairs);
  manyforge::core::ContentHasher geometry;
  geometry.mix("manyforge.step18.runtime-contact-envelope.v2")
      .mix(1.0)
      .mix(3.0)
      .mix(0.0)
      .mix(0.0)
      .mix(0.75)
      .mix(static_cast<std::uint64_t>(kDistanceDistractorCount))
      .mix(kDistanceDistractorExtentM);
  for (std::size_t index = 0; index < kDistanceDistractorCount; ++index) {
    geometry.mix(5.0 + static_cast<double>(index) * 0.25)
        .mix(5.0)
        .mix(5.0);
  }
  inputs.geometry_fingerprint = geometry.finalize();
  return inputs;
}

RuntimeCollisionStateProjection identity_projection(
    const RobotStateDTO& state) {
  RuntimeCollisionStateProjection projection;
  projection.source_joint_names = state.joint_names;
  projection.query_joint_names = state.joint_names;
  projection.template_positions_rad = state.positions_rad;
  projection.source_to_query_indices.resize(state.joint_names.size());
  std::iota(projection.source_to_query_indices.begin(),
            projection.source_to_query_indices.end(), 0);
  return projection;
}

TrajectoryDTO static_lookahead_trajectory(const RobotStateDTO& state) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = state.joint_names;
  trajectory.waypoints.reserve(kTrajectoryWaypointCount);
  for (std::size_t index = 0; index < kTrajectoryWaypointCount; ++index) {
    TrajectoryWaypointDTO waypoint;
    waypoint.time_from_start_s =
        static_cast<double>(index) * kTrajectoryStepS;
    waypoint.positions_rad = state.positions_rad;
    waypoint.velocities_rad_s = state.velocities_rad_s;
    trajectory.waypoints.push_back(std::move(waypoint));
  }
  return trajectory;
}

std::string trajectory_fingerprint(const TrajectoryDTO& trajectory) {
  manyforge::core::ContentHasher hasher;
  hasher.mix("manyforge.step18.runtime-static-trajectory.v1");
  for (const auto& name : trajectory.joint_names) {
    hasher.mix(name);
  }
  for (const auto& waypoint : trajectory.waypoints) {
    hasher.mix(waypoint.time_from_start_s);
    for (const double position : waypoint.positions_rad) {
      hasher.mix(position);
    }
  }
  return hasher.finalize();
}

struct MechanismSnapshot {
  std::uint64_t query_count = 0;
  std::uint64_t collision_api_calls = 0;
  std::uint64_t active_collision_pairs = 0;
  std::uint64_t colliding_geometry_pairs = 0;
  std::uint64_t colliding_logical_pairs = 0;
  std::uint64_t distance_api_calls = 0;
  std::uint64_t distance_geometry_pairs = 0;
  std::uint64_t distance_logical_pairs = 0;
  std::uint64_t fk_placement_invocations = 0;
  std::uint64_t undeclared_colliding_logical_pairs = 0;
  std::uint64_t distance_mode_mask = 0;
  std::uint64_t collision_completion_ordinal_failures = 0;
};

MechanismSnapshot subtract(const MechanismSnapshot& after,
                           const MechanismSnapshot& before) {
  MechanismSnapshot result;
#define MANYFORGE_SUBTRACT_FIELD(field) result.field = after.field - before.field
  MANYFORGE_SUBTRACT_FIELD(query_count);
  MANYFORGE_SUBTRACT_FIELD(collision_api_calls);
  MANYFORGE_SUBTRACT_FIELD(active_collision_pairs);
  MANYFORGE_SUBTRACT_FIELD(colliding_geometry_pairs);
  MANYFORGE_SUBTRACT_FIELD(colliding_logical_pairs);
  MANYFORGE_SUBTRACT_FIELD(distance_api_calls);
  MANYFORGE_SUBTRACT_FIELD(distance_geometry_pairs);
  MANYFORGE_SUBTRACT_FIELD(distance_logical_pairs);
  MANYFORGE_SUBTRACT_FIELD(fk_placement_invocations);
  MANYFORGE_SUBTRACT_FIELD(undeclared_colliding_logical_pairs);
  MANYFORGE_SUBTRACT_FIELD(collision_completion_ordinal_failures);
#undef MANYFORGE_SUBTRACT_FIELD
  result.distance_mode_mask = after.distance_mode_mask;
  return result;
}

class MechanismAccumulator final {
 public:
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  void add(const ManyForgeCollisionEngine::QueryMechanismCounters& counters,
           std::uint64_t undeclared_pairs) noexcept {
    query_count_.fetch_add(1, std::memory_order_relaxed);
    collision_api_calls_.fetch_add(counters.collision_api_call_count,
                                   std::memory_order_relaxed);
    active_collision_pairs_.fetch_add(counters.active_collision_pair_count,
                                      std::memory_order_relaxed);
    colliding_geometry_pairs_.fetch_add(
        counters.colliding_geometry_pair_count, std::memory_order_relaxed);
    colliding_logical_pairs_.fetch_add(
        counters.colliding_logical_pair_count, std::memory_order_relaxed);
    distance_api_calls_.fetch_add(counters.distance_api_call_count,
                                  std::memory_order_relaxed);
    distance_geometry_pairs_.fetch_add(
        counters.distance_evaluated_geometry_pair_count,
        std::memory_order_relaxed);
    distance_logical_pairs_.fetch_add(
        counters.distance_evaluated_logical_pair_count,
        std::memory_order_relaxed);
    fk_placement_invocations_.fetch_add(
        counters.fk_placement_invocation_count, std::memory_order_relaxed);
    undeclared_colliding_logical_pairs_.fetch_add(
        undeclared_pairs, std::memory_order_relaxed);
    distance_mode_mask_.fetch_or(
        std::uint64_t{1}
            << static_cast<unsigned>(counters.distance_request_mode),
        std::memory_order_relaxed);
    if (counters.collision_api_call_count > 0 &&
        counters.collision_enumeration_completion_ordinal != 1) {
      collision_completion_ordinal_failures_.fetch_add(
          1, std::memory_order_relaxed);
    }
  }
#endif

  MechanismSnapshot snapshot() const noexcept {
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
    return {
        query_count_.load(std::memory_order_relaxed),
        collision_api_calls_.load(std::memory_order_relaxed),
        active_collision_pairs_.load(std::memory_order_relaxed),
        colliding_geometry_pairs_.load(std::memory_order_relaxed),
        colliding_logical_pairs_.load(std::memory_order_relaxed),
        distance_api_calls_.load(std::memory_order_relaxed),
        distance_geometry_pairs_.load(std::memory_order_relaxed),
        distance_logical_pairs_.load(std::memory_order_relaxed),
        fk_placement_invocations_.load(std::memory_order_relaxed),
        undeclared_colliding_logical_pairs_.load(std::memory_order_relaxed),
        distance_mode_mask_.load(std::memory_order_relaxed),
        collision_completion_ordinal_failures_.load(
            std::memory_order_relaxed),
    };
#else
    return {};
#endif
  }

 private:
  std::atomic<std::uint64_t> query_count_{0};
  std::atomic<std::uint64_t> collision_api_calls_{0};
  std::atomic<std::uint64_t> active_collision_pairs_{0};
  std::atomic<std::uint64_t> colliding_geometry_pairs_{0};
  std::atomic<std::uint64_t> colliding_logical_pairs_{0};
  std::atomic<std::uint64_t> distance_api_calls_{0};
  std::atomic<std::uint64_t> distance_geometry_pairs_{0};
  std::atomic<std::uint64_t> distance_logical_pairs_{0};
  std::atomic<std::uint64_t> fk_placement_invocations_{0};
  std::atomic<std::uint64_t> undeclared_colliding_logical_pairs_{0};
  std::atomic<std::uint64_t> distance_mode_mask_{0};
  std::atomic<std::uint64_t> collision_completion_ordinal_failures_{0};
};

#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
class EvidenceEngine final : public ICollisionQueryEngine {
 public:
  EvidenceEngine(
      std::shared_ptr<ICollisionQueryEngine> delegate,
      allocation_probe::Domain engine_domain,
      std::shared_ptr<MechanismAccumulator> accumulator,
      std::shared_ptr<const std::vector<CollisionPairId>> allowed_pairs)
      : delegate_(std::move(delegate)),
        concrete_(dynamic_cast<ManyForgeCollisionEngine*>(delegate_.get())),
        engine_domain_(engine_domain),
        accumulator_(std::move(accumulator)),
        allowed_pairs_(std::move(allowed_pairs)) {
    if (!delegate_ || concrete_ == nullptr) {
      throw std::invalid_argument(
          "observation engine requires ManyForgeCollisionEngine delegate");
    }
  }

  const CollisionEngineCapabilityDescriptor& capability() const override {
    return delegate_->capability();
  }

  CollisionResult check(const manyforge::core::CollisionWorldSnapshot& world,
                        const CollisionQuery& query,
                        const CollisionPolicySnapshot& policy,
                        const CollisionCancellationToken& cancel) override {
    allocation_probe::Scope scope(engine_domain_);
    auto result = delegate_->check(world, query, policy, cancel);
    std::uint64_t undeclared = 0;
    for (const auto& pair : result.normalized_pairs) {
      if (!std::binary_search(allowed_pairs_->begin(), allowed_pairs_->end(),
                              pair)) {
        ++undeclared;
      }
    }
    accumulator_->add(concrete_->internal_last_query_mechanism_counters(),
                      undeclared);
    return result;
  }

  RuntimeCollisionQueryPrepareStatus prepare_runtime_query(
      std::span<const std::string> names) noexcept override {
    allocation_probe::Scope scope(engine_domain_);
    return delegate_->prepare_runtime_query(names);
  }

  RuntimeCollisionQueryPrepareStatus prepare_runtime_snapshot(
      const manyforge::core::CollisionWorldSnapshot& world) noexcept override {
    allocation_probe::Scope scope(engine_domain_);
    return delegate_->prepare_runtime_snapshot(world);
  }

  RuntimeCollisionQueryResultView check_runtime(
      const manyforge::core::CollisionWorldSnapshot& world,
      const RuntimeCollisionQueryRequest& request) noexcept override {
    allocation_probe::Scope scope(engine_domain_);
    const auto result = delegate_->check_runtime(world, request);
    std::uint64_t undeclared = 0;
    for (const auto& observation : result.collision_pairs) {
      if (observation.pair != nullptr &&
          !std::binary_search(allowed_pairs_->begin(), allowed_pairs_->end(),
                              *observation.pair)) {
        ++undeclared;
      }
    }
    accumulator_->add(concrete_->internal_last_query_mechanism_counters(),
                      undeclared);
    return result;
  }

  std::string engine_id() const override { return delegate_->engine_id(); }
  std::string engine_version() const override {
    return delegate_->engine_version();
  }

 private:
  std::shared_ptr<ICollisionQueryEngine> delegate_;
  ManyForgeCollisionEngine* concrete_ = nullptr;
  allocation_probe::Domain engine_domain_;
  std::shared_ptr<MechanismAccumulator> accumulator_;
  std::shared_ptr<const std::vector<CollisionPairId>> allowed_pairs_;
};
#endif

std::shared_ptr<ICollisionQueryEngine> evidence_engine(
    std::shared_ptr<ICollisionQueryEngine> source,
    allocation_probe::Domain domain,
    const std::shared_ptr<MechanismAccumulator>& accumulator,
    const std::shared_ptr<const std::vector<CollisionPairId>>& allowed_pairs) {
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  return std::make_shared<EvidenceEngine>(std::move(source), domain,
                                          accumulator, allowed_pairs);
#else
  (void)domain;
  (void)accumulator;
  (void)allowed_pairs;
  return source;
#endif
}

struct ReplayTranscriptRecord {
  std::int64_t waypoint_index = -1;
  bool in_collision = false;
  int engine_status = 0;
  int decision = 0;
  std::vector<CollisionPairId> normalized_pairs;
};

class ReplayTranscriptEngine final : public ICollisionQueryEngine {
 public:
  ReplayTranscriptEngine(
      std::shared_ptr<ICollisionQueryEngine> delegate,
      std::shared_ptr<MechanismAccumulator> accumulator,
      std::shared_ptr<const std::vector<CollisionPairId>> allowed_pairs)
      : delegate_(std::move(delegate)),
        accumulator_(std::move(accumulator)),
        allowed_pairs_(std::move(allowed_pairs)) {
    if (!delegate_ || !accumulator_ || !allowed_pairs_) {
      throw std::invalid_argument(
          "mechanism replay transcript engine inputs are incomplete");
    }
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
    concrete_ = dynamic_cast<ManyForgeCollisionEngine*>(delegate_.get());
    if (concrete_ == nullptr) {
      throw std::invalid_argument(
          "mechanism replay observation requires ManyForgeCollisionEngine");
    }
#endif
  }

  const CollisionEngineCapabilityDescriptor& capability() const override {
    return delegate_->capability();
  }

  CollisionResult check(const manyforge::core::CollisionWorldSnapshot& world,
                        const CollisionQuery& query,
                        const CollisionPolicySnapshot& policy,
                        const CollisionCancellationToken& cancel) override {
    auto result = delegate_->check(world, query, policy, cancel);
    ReplayTranscriptRecord record;
    record.waypoint_index = query.waypoint_index.value_or(-1);
    record.in_collision = result.in_collision;
    record.engine_status = static_cast<int>(result.engine_status);
    record.decision = static_cast<int>(result.decision);
    record.normalized_pairs = result.normalized_pairs;
    std::sort(record.normalized_pairs.begin(), record.normalized_pairs.end());
    record.normalized_pairs.erase(
        std::unique(record.normalized_pairs.begin(),
                    record.normalized_pairs.end()),
        record.normalized_pairs.end());
    std::uint64_t undeclared = 0;
    for (const auto& pair : record.normalized_pairs) {
      if (!std::binary_search(allowed_pairs_->begin(), allowed_pairs_->end(),
                              pair)) {
        ++undeclared;
      }
    }
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
    accumulator_->add(concrete_->internal_last_query_mechanism_counters(),
                      undeclared);
#else
    (void)undeclared;
#endif
    records_.push_back(std::move(record));
    return result;
  }

  RuntimeCollisionQueryPrepareStatus prepare_runtime_query(
      std::span<const std::string> names) noexcept override {
    return delegate_->prepare_runtime_query(names);
  }

  RuntimeCollisionQueryPrepareStatus prepare_runtime_snapshot(
      const manyforge::core::CollisionWorldSnapshot& world) noexcept override {
    return delegate_->prepare_runtime_snapshot(world);
  }

  RuntimeCollisionQueryResultView check_runtime(
      const manyforge::core::CollisionWorldSnapshot& world,
      const RuntimeCollisionQueryRequest& request) noexcept override {
    return delegate_->check_runtime(world, request);
  }

  std::string engine_id() const override { return delegate_->engine_id(); }
  std::string engine_version() const override {
    return delegate_->engine_version();
  }

  const std::vector<ReplayTranscriptRecord>& records() const noexcept {
    return records_;
  }

 private:
  std::shared_ptr<ICollisionQueryEngine> delegate_;
  std::shared_ptr<MechanismAccumulator> accumulator_;
  std::shared_ptr<const std::vector<CollisionPairId>> allowed_pairs_;
  std::vector<ReplayTranscriptRecord> records_;
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  ManyForgeCollisionEngine* concrete_ = nullptr;
#endif
};

std::string replay_transcript_fingerprint(
    const std::vector<ReplayTranscriptRecord>& records) {
  manyforge::core::ContentHasher hash;
  hash.mix("manyforge.step18.isaac-live-pair-transcript.v1");
  for (const auto& record : records) {
    hash.mix(static_cast<std::uint64_t>(record.waypoint_index))
        .mix(static_cast<std::uint64_t>(record.in_collision))
        .mix(static_cast<std::uint64_t>(record.engine_status));
    for (const auto& pair : record.normalized_pairs) {
      hash.mix(pair.body_a).mix(pair.body_b);
    }
  }
  return hash.finalize();
}

void append_replay_transcript(
    std::ostringstream& out,
    const std::vector<ReplayTranscriptRecord>& records) {
  out << '[';
  for (std::size_t index = 0; index < records.size(); ++index) {
    if (index != 0) {
      out << ',';
    }
    const auto& record = records[index];
    out << "{\"waypoint_index\":" << record.waypoint_index
        << ",\"in_collision\":" << json_bool(record.in_collision)
        << ",\"engine_status\":" << record.engine_status
        << ",\"decision\":" << record.decision
        << ",\"normalized_pairs\":[";
    for (std::size_t pair_index = 0;
         pair_index < record.normalized_pairs.size(); ++pair_index) {
      if (pair_index != 0) {
        out << ',';
      }
      const auto& pair = record.normalized_pairs[pair_index];
      out << "{\"body_a\":\"" << json_escape(pair.body_a)
          << "\",\"body_b\":\"" << json_escape(pair.body_b) << "\"}";
    }
    out << "]}";
  }
  out << ']';
}

std::string mechanism_replay_result(const Options& options) {
  ptree capture;
  boost::property_tree::read_json(options.mechanism_replay_input.string(),
                                  capture);
  if (capture.get<std::string>("schema_version") !=
      "manyforge.roboplan_transition_capture.v1") {
    throw std::invalid_argument("unsupported mechanism replay input schema");
  }
  const auto urdf = options.repo_root /
                    "manyforge_example_models/models/ur10e_robotiq_2f_140/"
                    "ur10e_robotiq_2f_140.urdf";
  const auto srdf = options.repo_root /
                    "manyforge_example_models/models/ur10e_robotiq_2f_140/"
                    "ur10e_robotiq_2f_140.srdf";
  auto native = create_native_kernel(
      "ur10e", "arm", urdf.string(), srdf.string(),
      std::vector<std::string>{options.repo_root.string()});
  if (!native || !native->scene_authority ||
      !native->collision_trajectory_projector ||
      native->collision_validation_engines.empty() ||
      !native->collision_validation_engines.front()) {
    throw std::runtime_error(
        "failed to create Isaac/UR10 mechanism replay bundle");
  }
  seed_replay_projection(*native, capture);
  configure_replay_world(*native, capture, options);
  const auto input_trajectory = replay_trajectory(capture);
  const auto start_state = replay_start_state(capture);
  auto expanded_result = native->collision_trajectory_projector->
      expand_for_collision(input_trajectory, start_state);
  if (!expanded_result.ok()) {
    throw std::runtime_error(
        "mechanism replay collision projection failed: " +
        expanded_result.error().message);
  }
  auto expanded = std::move(expanded_result.value());
  const auto bundle =
      native->scene_authority->published_runtime_collision_query_bundle();
  if (!bundle || !bundle->world || !bundle->policy || !bundle->policy_index) {
    throw std::runtime_error(
        "mechanism replay did not publish a coherent world-policy bundle");
  }
  auto allowed = std::make_shared<std::vector<CollisionPairId>>();
  for (const auto& entry : bundle->policy->support_contacts) {
    allowed->push_back(entry.pair);
  }
  std::sort(allowed->begin(), allowed->end());
  allowed->erase(std::unique(allowed->begin(), allowed->end()), allowed->end());
  if (allowed->empty()) {
    throw std::runtime_error(
        "mechanism replay has no configured support-contact pair");
  }
  auto immutable_allowed =
      std::shared_ptr<const std::vector<CollisionPairId>>(std::move(allowed));
  auto accumulator = std::make_shared<MechanismAccumulator>();
  auto transcript_engine = std::make_shared<ReplayTranscriptEngine>(
      native->collision_validation_engines.front(), accumulator,
      immutable_allowed);
  CollisionValidationPassConfig pass;
  pass.policy_snapshot_ref = bundle->policy;
  pass.policy_index = bundle->policy_index;
  pass.require_pose_source_borrow = true;
  pass.per_state_timeout_ns = 250'000'000;
  pass.surface = CollisionSurface::kPreExecutionValidation;
  CollisionValidationCoordinator coordinator(std::move(pass));
  CollisionCancellationToken cancel;
  const auto decision = coordinator.validate_trajectory(
      *bundle->world, *transcript_engine, expanded, cancel);
  const auto& records = transcript_engine->records();
  if (!decision.decision.accepted || decision.cancelled ||
      records.size() != expanded.waypoints.size()) {
    throw std::runtime_error(
        "mechanism replay did not reproduce the accepted complete live "
        "validation trajectory");
  }
  const auto mechanism = accumulator->snapshot();
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  if (mechanism.query_count != records.size() ||
      mechanism.collision_api_calls != records.size() ||
      mechanism.collision_completion_ordinal_failures != 0) {
    throw std::runtime_error(
        "mechanism replay counters do not prove complete collision enumeration");
  }
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
  if (mechanism.distance_mode_mask != (std::uint64_t{1} << 3) ||
      mechanism.distance_api_calls == 0) {
    throw std::runtime_error(
        "baseline mechanism replay did not use all-active-pair distance");
  }
#else
  if (options.arm == Arm::kBinaryContact) {
    if (mechanism.distance_mode_mask != 1 ||
        mechanism.distance_api_calls != 0 ||
        mechanism.distance_geometry_pairs != 0 ||
        mechanism.distance_logical_pairs != 0) {
      throw std::runtime_error(
          "binary mechanism replay performed forbidden distance work");
    }
  } else if (
      mechanism.distance_mode_mask != (std::uint64_t{1} << 1) ||
      mechanism.distance_logical_pairs > mechanism.colliding_logical_pairs) {
    throw std::runtime_error(
        "metric mechanism replay measured outside colliding configured pairs");
  }
#endif
#endif

  std::ostringstream out;
  out << "{\"schema_version\":\"manyforge.step18_isaac_live_mechanism_replay.v1\""
      << ",\"run_id\":\"" << json_escape(options.run_id) << "\""
      << ",\"arm\":\"" << arm_name(options.arm) << "\""
      << ",\"measurement_kind\":\""
      << (MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION ? "observation" : "decision")
      << "\",\"source_role\":\"" << json_escape(options.source_role)
      << "\",\"input\":{\"capture_id\":\""
      << json_escape(capture.get<std::string>("capture_id"))
      << "\",\"semantic_sha256\":\""
      << json_escape(capture.get<std::string>("semantic_sha256"))
      << "\",\"trajectory_sha256\":\""
      << json_escape(capture.get<std::string>(
             "planning_result.trajectory_sha256"))
      << "\",\"projection_seed_sha256\":\""
      << json_escape(capture.get<std::string>(
             "scene.robot.collision_projection_seed.sha256"))
      << "\",\"captured_scene_content_hash\":\""
      << json_escape(capture.get<std::string>(
             "scene.authority.revision.content_hash"))
      << "\"},\"replay\":{\"world_content_hash\":\""
      << json_escape(bundle->world->revision.content_hash)
      << "\",\"policy_content_hash\":\""
      << json_escape(bundle->policy->revision.content_hash)
      << "\",\"input_waypoint_count\":"
      << input_trajectory.waypoints.size()
      << ",\"expanded_waypoint_count\":" << expanded.waypoints.size()
      << ",\"expanded_joint_count\":" << expanded.joint_names.size()
      << "},\"policy_contract\":{\"support_contacts\":[";
  for (std::size_t index = 0; index < bundle->policy->support_contacts.size();
       ++index) {
    if (index != 0) {
      out << ',';
    }
    const auto& entry = bundle->policy->support_contacts[index];
    out << "{\"pair\":{\"body_a\":\"" << json_escape(entry.pair.body_a)
        << "\",\"body_b\":\"" << json_escape(entry.pair.body_b)
        << "\"},\"mode\":" << static_cast<int>(entry.mode)
        << ",\"release_evidence_mode\":"
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
        << 0
#else
        << static_cast<int>(entry.release_evidence_mode)
#endif
        << ",\"enforce_max_start_penetration\":"
        << json_bool(entry.enforce_max_start_penetration)
        << ",\"max_start_penetration_m\":"
        << entry.max_start_penetration_m
        << ",\"release_tolerance_m\":" << entry.release_tolerance_m
        << ",\"min_release_progress_m\":" << entry.min_release_progress_m
        << ",\"max_contact_penetration_m\":"
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
        << 0.0
#else
        << entry.max_contact_penetration_m
#endif
        << ",\"require_separation_by_end\":"
        << json_bool(entry.require_separation_by_end)
        << ",\"require_strictly_decreasing_penetration\":"
        << json_bool(entry.require_strictly_decreasing_penetration)
        << ",\"allow_recontact\":" << json_bool(entry.allow_recontact)
        << '}';
  }
  out << "]},\"decision\":{\"accepted\":"
      << json_bool(decision.decision.accepted)
      << ",\"action\":" << static_cast<int>(decision.decision.action)
      << ",\"first_failing_waypoint\":";
  if (decision.decision.first_failing_waypoint) {
    out << *decision.decision.first_failing_waypoint;
  } else {
    out << "null";
  }
  out << "},\"mechanism\":{\"query_count\":" << mechanism.query_count
      << ",\"collision_api_calls\":" << mechanism.collision_api_calls
      << ",\"active_collision_pairs\":" << mechanism.active_collision_pairs
      << ",\"colliding_geometry_pairs\":"
      << mechanism.colliding_geometry_pairs
      << ",\"colliding_logical_pairs\":"
      << mechanism.colliding_logical_pairs
      << ",\"distance_api_calls\":" << mechanism.distance_api_calls
      << ",\"distance_geometry_pairs\":"
      << mechanism.distance_geometry_pairs
      << ",\"distance_logical_pairs\":"
      << mechanism.distance_logical_pairs
      << ",\"fk_placement_invocations\":"
      << mechanism.fk_placement_invocations
      << ",\"undeclared_colliding_logical_pairs\":"
      << mechanism.undeclared_colliding_logical_pairs
      << ",\"distance_mode_mask\":" << mechanism.distance_mode_mask
      << ",\"collision_completion_ordinal_failures\":"
      << mechanism.collision_completion_ordinal_failures
      << "},\"pair_transcript_fingerprint\":\""
      << replay_transcript_fingerprint(records)
      << "\",\"pair_transcript\":";
  append_replay_transcript(out, records);
  out << '}';
  return out.str();
}

struct ColdSyncEvidence {
  RuntimeCollisionQueryPrepareStatus status =
      RuntimeCollisionQueryPrepareStatus::kEngineError;
  std::int64_t wall_ns = 0;
  std::int64_t process_cpu_ns = 0;
  std::int64_t thread_cpu_ns = 0;
  std::uint64_t immediate_engine_allocations = 0;
  std::uint64_t lookahead_engine_allocations = 0;
  std::uint64_t scene_build_count_before = 0;
  std::uint64_t scene_build_count_after = 0;
};

ColdSyncEvidence measure_snapshot_prepare(
    ICollisionQueryEngine& engine,
    const std::shared_ptr<ICollisionQueryEngine>& raw_engine,
    const manyforge::core::CollisionWorldSnapshot& world) {
  ColdSyncEvidence evidence;
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  auto* concrete = dynamic_cast<ManyForgeCollisionEngine*>(raw_engine.get());
  if (concrete != nullptr) {
    evidence.scene_build_count_before =
        concrete->internal_prepared_scene_build_count();
  }
#else
  (void)raw_engine;
#endif
  const auto immediate_before =
      allocation_probe::load(allocation_probe::Domain::kImmediateEngine);
  const auto lookahead_before =
      allocation_probe::load(allocation_probe::Domain::kLookaheadEngine);
  const auto process_before = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
  const auto thread_before = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  const auto wall_before = wall_now_ns();
  evidence.status = engine.prepare_runtime_snapshot(world);
  evidence.wall_ns = wall_now_ns() - wall_before;
  evidence.thread_cpu_ns =
      clock_ns(CLOCK_THREAD_CPUTIME_ID) - thread_before;
  evidence.process_cpu_ns =
      clock_ns(CLOCK_PROCESS_CPUTIME_ID) - process_before;
  evidence.immediate_engine_allocations =
      allocation_probe::load(allocation_probe::Domain::kImmediateEngine) -
      immediate_before;
  evidence.lookahead_engine_allocations =
      allocation_probe::load(allocation_probe::Domain::kLookaheadEngine) -
      lookahead_before;
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  if (concrete != nullptr) {
    evidence.scene_build_count_after =
        concrete->internal_prepared_scene_build_count();
  }
#endif
  return evidence;
}

struct ImmediateSample {
  std::int64_t sequence = 0;
  std::int64_t callback_started_at_ns = 0;
  std::int64_t callback_finished_at_ns = 0;
  std::int64_t started_at_ns = 0;
  std::int64_t finished_at_ns = 0;
  std::int64_t tick_wall_ns = 0;
  std::int64_t callback_wall_ns = 0;
  std::int64_t process_cpu_ns = 0;
  std::int64_t thread_cpu_ns = 0;
  std::int64_t engine_query_ns = 0;
  RuntimeCollisionTickStatus status = RuntimeCollisionTickStatus::kInconclusive;
  int reason = 0;
  std::uint64_t framework_allocations = 0;
  std::uint64_t engine_allocations = 0;
  MechanismSnapshot mechanism;
};

struct LookaheadSample {
  std::int64_t requested_at_ns = 0;
  std::int64_t callback_at_ns = 0;
  std::int64_t cycle_wall_ns = 0;
  std::int64_t worker_active_wall_ns = 0;
  std::int64_t process_cpu_ns = -1;
  std::int64_t start_jitter_ns = 0;
  std::size_t checked_samples = 0;
  int workers_used = 0;
  RuntimeCollisionLookaheadResultStatus status =
      RuntimeCollisionLookaheadResultStatus::kInconclusive;
  bool cycle_deadline_exceeded = false;
  bool process_stamp_found = false;
  MechanismSnapshot mechanism;
};

struct RequestStamp {
  std::atomic<std::int64_t> requested_at_ns{-1};
  std::atomic<std::int64_t> process_cpu_ns{-1};
};

class RequestStampRing final {
 public:
  explicit RequestStampRing(std::size_t capacity)
      : capacity_(capacity), stamps_(std::make_unique<RequestStamp[]>(capacity)) {}

  void record(std::int64_t requested_at_ns,
              std::int64_t process_cpu_ns) noexcept {
    const auto index = next_.fetch_add(1, std::memory_order_relaxed) % capacity_;
    stamps_[index].process_cpu_ns.store(process_cpu_ns,
                                        std::memory_order_relaxed);
    stamps_[index].requested_at_ns.store(requested_at_ns,
                                         std::memory_order_release);
  }

  std::optional<std::int64_t> find(std::int64_t requested_at_ns) const noexcept {
    for (std::size_t index = 0; index < capacity_; ++index) {
      if (stamps_[index].requested_at_ns.load(std::memory_order_acquire) ==
          requested_at_ns) {
        return stamps_[index].process_cpu_ns.load(std::memory_order_relaxed);
      }
    }
    return std::nullopt;
  }

 private:
  std::size_t capacity_ = 0;
  std::unique_ptr<RequestStamp[]> stamps_;
  std::atomic<std::size_t> next_{0};
};

enum class CampaignPhase {
  kWarmup,
  kWarmupComplete,
  kMeasured,
  kDone,
  kFailed,
};

struct CampaignState {
  explicit CampaignState(const Options& options)
      : immediate_samples(static_cast<std::size_t>(
            options.measured_ticks + options.max_extra_ticks)),
        lookahead_samples(static_cast<std::size_t>(
            options.measured_ticks + options.max_extra_ticks)),
        request_stamps(static_cast<std::size_t>(
            options.measured_ticks + options.max_extra_ticks + 64)) {}

  std::atomic<CampaignPhase> phase{CampaignPhase::kWarmup};
  std::atomic<bool> monitor_continue{true};
  std::atomic<int> warmup_ticks{0};
  std::atomic<int> warmup_lookahead{0};
  std::atomic<int> warmup_lookahead_requests{0};
  std::atomic<int> measured_ticks{0};
  std::atomic<int> measured_lookahead{0};
  std::atomic<int> measured_lookahead_requests{0};
  std::atomic<int> immediate_faults{0};
  std::atomic<int> lookahead_faults{0};
  std::atomic<int> exact_32_failures{0};
  std::atomic<int> immediate_sample_count{0};
  std::atomic<int> lookahead_sample_count{0};
  std::vector<ImmediateSample> immediate_samples;
  std::vector<LookaheadSample> lookahead_samples;
  RequestStampRing request_stamps;
  MechanismSnapshot last_lookahead_mechanism;
  std::mutex lookahead_mechanism_mutex;
  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  std::string failure_reason;
};

bool reserve_lookahead_request(CampaignState& state, CampaignPhase phase,
                               const Options& options) noexcept {
  std::atomic<int>* issued = nullptr;
  const std::atomic<int>* completed = nullptr;
  int target = 0;
  if (phase == CampaignPhase::kWarmup) {
    issued = &state.warmup_lookahead_requests;
    completed = &state.warmup_lookahead;
    target = options.warmup_lookahead_cycles;
  } else if (phase == CampaignPhase::kMeasured) {
    issued = &state.measured_lookahead_requests;
    completed = &state.measured_lookahead;
    target = options.measured_lookahead_cycles;
  } else {
    return false;
  }

  int current = issued->load(std::memory_order_acquire);
  while (current < target) {
    // Keep at most one cycle outstanding. In particular, this closes the gap
    // where the session has marked a job idle but its result callback has not
    // yet committed the completed-cycle count.
    if (current != completed->load(std::memory_order_acquire)) {
      return false;
    }
    if (issued->compare_exchange_weak(current, current + 1,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

struct Distribution {
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double maximum = 0.0;
};

Distribution distribution(std::vector<std::int64_t> values) {
  values.erase(std::remove_if(values.begin(), values.end(),
                              [](std::int64_t value) { return value < 0; }),
               values.end());
  if (values.empty()) {
    return {};
  }
  std::sort(values.begin(), values.end());
  auto percentile = [&](double quantile) {
    const double rank = quantile * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(rank);
    const auto upper = std::min(lower + 1, values.size() - 1);
    const double fraction = rank - static_cast<double>(lower);
    return static_cast<double>(values[lower]) * (1.0 - fraction) +
           static_cast<double>(values[upper]) * fraction;
  };
  return {percentile(0.50), percentile(0.95), percentile(0.99),
          percentile(1.0)};
}

void append_distribution(std::ostringstream& out,
                         const Distribution& value) {
  out << "{\"p50_ns\":" << value.p50 << ",\"p95_ns\":" << value.p95
      << ",\"p99_ns\":" << value.p99 << ",\"max_ns\":"
      << value.maximum << "}";
}

void append_mechanism(std::ostringstream& out,
                      const MechanismSnapshot& value) {
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  out << "{\"evidence_kind\":\"direct_counter\",\"query_count\":"
      << value.query_count << ",\"collision_api_calls\":"
      << value.collision_api_calls << ",\"active_collision_pairs\":"
      << value.active_collision_pairs
      << ",\"colliding_geometry_pairs\":"
      << value.colliding_geometry_pairs
      << ",\"colliding_logical_pairs\":"
      << value.colliding_logical_pairs << ",\"distance_api_calls\":"
      << value.distance_api_calls << ",\"distance_geometry_pairs\":"
      << value.distance_geometry_pairs << ",\"distance_logical_pairs\":"
      << value.distance_logical_pairs << ",\"fk_placement_invocations\":"
      << value.fk_placement_invocations
      << ",\"undeclared_colliding_logical_pairs\":"
      << value.undeclared_colliding_logical_pairs
      << ",\"distance_mode_mask\":" << value.distance_mode_mask
      << ",\"collision_completion_ordinal_failures\":"
      << value.collision_completion_ordinal_failures << "}";
#else
  (void)value;
  out << "{\"evidence_kind\":\"not_measured\",\"query_count\":null"
         ",\"collision_api_calls\":null,\"active_collision_pairs\":null"
         ",\"colliding_geometry_pairs\":null"
         ",\"colliding_logical_pairs\":null,\"distance_api_calls\":null"
         ",\"distance_geometry_pairs\":null"
         ",\"distance_logical_pairs\":null"
         ",\"fk_placement_invocations\":null"
         ",\"undeclared_colliding_logical_pairs\":null"
         ",\"distance_mode_mask\":null"
         ",\"collision_completion_ordinal_failures\":null}";
#endif
}

const char* immediate_status_name(RuntimeCollisionTickStatus status) {
  switch (status) {
    case RuntimeCollisionTickStatus::kClear:
      return "clear";
    case RuntimeCollisionTickStatus::kCollision:
      return "collision";
    case RuntimeCollisionTickStatus::kInconclusive:
      return "inconclusive";
  }
  return "unknown";
}

const char* lookahead_status_name(RuntimeCollisionLookaheadResultStatus status) {
  switch (status) {
    case RuntimeCollisionLookaheadResultStatus::kClear:
      return "clear";
    case RuntimeCollisionLookaheadResultStatus::kCollision:
      return "collision";
    case RuntimeCollisionLookaheadResultStatus::kInconclusive:
      return "inconclusive";
    case RuntimeCollisionLookaheadResultStatus::kSupersededRevision:
      return "superseded_revision";
    case RuntimeCollisionLookaheadResultStatus::kNoFutureSamples:
      return "no_future_samples";
  }
  return "unknown";
}

void notify_completion(CampaignState& state) {
  state.completion_cv.notify_all();
}

void fail_campaign(CampaignState& state, std::string reason) {
  {
    std::lock_guard<std::mutex> lock(state.completion_mutex);
    state.failure_reason = std::move(reason);
  }
  state.phase.store(CampaignPhase::kFailed, std::memory_order_release);
  state.monitor_continue.store(false, std::memory_order_release);
  notify_completion(state);
}

std::string campaign_failure_reason(CampaignState& state) {
  std::lock_guard<std::mutex> lock(state.completion_mutex);
  return state.failure_reason;
}

void complete_campaign_phase_if_ready(CampaignState& state,
                                      const Options& options) {
  auto phase = state.phase.load(std::memory_order_acquire);
  if (phase == CampaignPhase::kWarmup &&
      state.warmup_ticks.load(std::memory_order_relaxed) >=
          options.warmup_ticks &&
      state.warmup_lookahead.load(std::memory_order_relaxed) >=
          options.warmup_lookahead_cycles) {
    if (state.phase.compare_exchange_strong(
            phase, CampaignPhase::kWarmupComplete,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      state.monitor_continue.store(false, std::memory_order_release);
      notify_completion(state);
    }
    return;
  }
  if (phase == CampaignPhase::kMeasured &&
      state.measured_ticks.load(std::memory_order_relaxed) >=
          options.measured_ticks &&
      state.measured_lookahead.load(std::memory_order_relaxed) >=
          options.measured_lookahead_cycles) {
    if (state.phase.compare_exchange_strong(
            phase, CampaignPhase::kDone, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      state.monitor_continue.store(false, std::memory_order_release);
      notify_completion(state);
    }
  }
}

bool wait_for_phase_end(CampaignState& state, CampaignPhase phase,
                        int timeout_s) {
  std::unique_lock<std::mutex> lock(state.completion_mutex);
  return state.completion_cv.wait_for(
      lock, std::chrono::seconds(timeout_s), [&]() {
        return state.phase.load(std::memory_order_acquire) != phase;
      });
}

bool wait_for_lookahead_idle(RuntimeCollisionLookaheadSession& lookahead,
                             int timeout_s) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(timeout_s);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto status = lookahead.status();
    if (!status.job_active && !status.permit_pending) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

struct RunEvidence {
  ColdSyncEvidence immediate_cold;
  ColdSyncEvidence immediate_warm_reuse;
  ColdSyncEvidence lookahead_cold;
  ColdSyncEvidence lookahead_warm_reuse;
  RuntimeCollisionMonitorTimingMetrics monitor_metrics;
  RuntimeCollisionLookaheadStatus lookahead_status;
  MechanismSnapshot immediate_mechanism;
  MechanismSnapshot lookahead_mechanism;
  int immediate_sample_count = 0;
  int lookahead_sample_count = 0;
  int immediate_faults = 0;
  int lookahead_faults = 0;
  int exact_32_failures = 0;
  int measured_lookahead_requests_issued = 0;
  int overlap_ticks = 0;
  std::vector<ImmediateSample> immediate_samples;
  std::vector<LookaheadSample> lookahead_samples;
};

RunEvidence run_campaign(NativeKernelBundle& native,
                         const FixtureInputs& fixture,
                         const TrajectoryDTO& trajectory,
                         const Options& options) {
  auto immediate_accumulator = std::make_shared<MechanismAccumulator>();
  auto lookahead_accumulator = std::make_shared<MechanismAccumulator>();
  auto immediate_engine = evidence_engine(
      native.collision_runtime_monitor_engine,
      allocation_probe::Domain::kImmediateEngine, immediate_accumulator,
      fixture.allowed_pairs);
  std::vector<std::shared_ptr<ICollisionQueryEngine>> lookahead_engines;
  lookahead_engines.reserve(native.collision_runtime_lookahead_engines.size());
  for (const auto& source : native.collision_runtime_lookahead_engines) {
    lookahead_engines.push_back(evidence_engine(
        source, allocation_probe::Domain::kLookaheadEngine,
        lookahead_accumulator, fixture.allowed_pairs));
  }
  if (lookahead_engines.size() != static_cast<std::size_t>(options.workers)) {
    throw std::runtime_error("native factory returned wrong lookahead capacity");
  }

  RunEvidence evidence;
  if (immediate_engine->prepare_runtime_query(fixture.state.joint_names) !=
      RuntimeCollisionQueryPrepareStatus::kReady) {
    throw std::runtime_error("immediate joint-context preparation failed");
  }
  evidence.immediate_cold = measure_snapshot_prepare(
      *immediate_engine, native.collision_runtime_monitor_engine,
      *fixture.bundle->world);
  evidence.immediate_warm_reuse = measure_snapshot_prepare(
      *immediate_engine, native.collision_runtime_monitor_engine,
      *fixture.bundle->world);
  evidence.lookahead_cold = measure_snapshot_prepare(
      *lookahead_engines.front(),
      native.collision_runtime_lookahead_engines.front(),
      *fixture.bundle->world);
  evidence.lookahead_warm_reuse = measure_snapshot_prepare(
      *lookahead_engines.front(),
      native.collision_runtime_lookahead_engines.front(),
      *fixture.bundle->world);
  const auto ready = [](const ColdSyncEvidence& value) {
    return value.status == RuntimeCollisionQueryPrepareStatus::kReady;
  };
  if (!ready(evidence.immediate_cold) ||
      !ready(evidence.immediate_warm_reuse) ||
      !ready(evidence.lookahead_cold) ||
      !ready(evidence.lookahead_warm_reuse)) {
    throw std::runtime_error("cold/warm snapshot preparation failed");
  }

  RuntimeCollisionQuerySession immediate(
      identity_projection(fixture.state), *native.scene_authority,
      immediate_engine, {}, kImmediateQueryBudgetNs);
  if (const auto error = immediate.prepare(fixture.bundle); error.has_value()) {
    throw std::runtime_error("immediate session prepare failed: " +
                             error->message);
  }

  ExecutionProgressPublication progress(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation,
      "step18_isolated", 0);
  const auto progress_time = native.clock->now();
  if (!progress.publish(1, 0, progress_time, progress_time).published) {
    throw std::runtime_error("failed to publish lookahead progress cursor");
  }

  CampaignState state(options);
  RuntimeCollisionLookaheadSession::Config lookahead_config;
  lookahead_config.cycle_budget_ns = kLookaheadCycleBudgetNs;
  lookahead_config.horizon_ns = kLookaheadHorizonNs;
  lookahead_config.max_samples = kLookaheadSamples;
  lookahead_config.workers = options.workers;
  lookahead_config.min_waypoints_for_parallel =
      kLookaheadMinParallelWaypoints;
  lookahead_config.query_budget_ns = kLookaheadQueryBudgetNs;

  RuntimeCollisionLookaheadSession lookahead(
      lookahead_config, *native.clock, *native.scheduler,
      *native.scene_authority, progress, lookahead_engines,
      [&](const RuntimeCollisionLookaheadSession*,
          RuntimeCollisionLookaheadResult result) {
        const auto callback_at_ns = native.clock->now();
        const auto process_at_callback = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
        MechanismSnapshot mechanism_delta;
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
        const auto mechanism_now = lookahead_accumulator->snapshot();
        {
          std::lock_guard<std::mutex> lock(state.lookahead_mechanism_mutex);
          mechanism_delta = subtract(mechanism_now,
                                     state.last_lookahead_mechanism);
          state.last_lookahead_mechanism = mechanism_now;
        }
#endif
        const auto phase = state.phase.load(std::memory_order_acquire);
        if (phase == CampaignPhase::kWarmup) {
          state.warmup_lookahead.fetch_add(1, std::memory_order_relaxed);
        } else if (phase == CampaignPhase::kMeasured) {
          const int index = state.lookahead_sample_count.fetch_add(
              1, std::memory_order_relaxed);
          if (index >= static_cast<int>(state.lookahead_samples.size())) {
            fail_campaign(state, "lookahead sample capacity exceeded");
            return;
          }
          auto& sample = state.lookahead_samples[static_cast<std::size_t>(index)];
          sample.requested_at_ns = result.requested_at_ns;
          sample.callback_at_ns = callback_at_ns;
          sample.cycle_wall_ns = result.cycle_duration_ns;
          sample.worker_active_wall_ns = result.worker_active_duration_ns;
          sample.start_jitter_ns = result.start_jitter_ns;
          sample.checked_samples = result.checked_samples;
          sample.workers_used = result.workers_used;
          sample.status = result.status;
          sample.cycle_deadline_exceeded = result.cycle_deadline_exceeded;
          sample.mechanism = mechanism_delta;
          if (const auto stamp = state.request_stamps.find(
                  result.requested_at_ns);
              stamp.has_value()) {
            sample.process_stamp_found = true;
            sample.process_cpu_ns = process_at_callback - *stamp;
          }
          state.measured_lookahead.fetch_add(1, std::memory_order_relaxed);
          if (result.checked_samples != kLookaheadSamples) {
            state.exact_32_failures.fetch_add(1,
                                              std::memory_order_relaxed);
          }
          if (result.status != RuntimeCollisionLookaheadResultStatus::kClear ||
              result.cycle_deadline_exceeded) {
            state.lookahead_faults.fetch_add(1, std::memory_order_relaxed);
          }
        }

        complete_campaign_phase_if_ready(state, options);
      },
      [](const RuntimeCollisionLookaheadSession*,
         RuntimeCollisionLookaheadResult&) {
        return manyforge::core::
            RuntimeCollisionLookaheadAuthorityDisposition::kAccepted;
      });
  lookahead.arm(1, 1, std::make_shared<const TrajectoryDTO>(trajectory), {});

  auto immediate_callback = [&]() -> manyforge::core::Result<void> {
    const auto phase = state.phase.load(std::memory_order_acquire);
    const auto mechanism_before = immediate_accumulator->snapshot();
    const auto framework_before = allocation_probe::load(
        allocation_probe::Domain::kImmediateFramework);
    const auto engine_before = allocation_probe::load(
        allocation_probe::Domain::kImmediateEngine);
    const auto callback_started = wall_now_ns();
    const auto process_started = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    const auto thread_started = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    RuntimeCollisionTickOutcome outcome;
    const auto tick_started = wall_now_ns();
    std::int64_t tick_finished = tick_started;
    {
      allocation_probe::Scope framework_scope(
          allocation_probe::Domain::kImmediateFramework);
      outcome = immediate.tick(immediate.acquire_bundle(),
                               fixture.state.positions_rad,
                               native.clock->now());
      tick_finished = wall_now_ns();
      if (outcome.status == RuntimeCollisionTickStatus::kClear &&
          reserve_lookahead_request(state, phase, options)) {
        const auto request_at = native.clock->now();
        if (phase == CampaignPhase::kMeasured) {
          allocation_probe::Scope observer_scope(
              allocation_probe::Domain::kUnmeasured);
          state.request_stamps.record(request_at,
                                      clock_ns(CLOCK_PROCESS_CPUTIME_ID));
        }
        lookahead.request(request_at, immediate.acquire_bundle(),
                          immediate.contact_release_state_snapshot());
      }
    }
    const auto thread_finished = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    const auto process_finished = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    const auto mechanism_after = immediate_accumulator->snapshot();

    if (outcome.status != RuntimeCollisionTickStatus::kClear) {
      state.immediate_faults.fetch_add(1, std::memory_order_relaxed);
    }
    const auto callback_finished = wall_now_ns();

    if (phase == CampaignPhase::kWarmup) {
      state.warmup_ticks.fetch_add(1, std::memory_order_relaxed);
    } else if (phase == CampaignPhase::kMeasured) {
      const int index = state.immediate_sample_count.fetch_add(
          1, std::memory_order_relaxed);
      if (index >= static_cast<int>(state.immediate_samples.size())) {
        fail_campaign(state, "immediate sample capacity exceeded");
        return manyforge::core::Result<void>::Ok();
      }
      auto& sample = state.immediate_samples[static_cast<std::size_t>(index)];
      sample.sequence = index + 1;
      sample.callback_started_at_ns = callback_started;
      sample.callback_finished_at_ns = callback_finished;
      sample.started_at_ns = tick_started;
      sample.finished_at_ns = tick_finished;
      sample.tick_wall_ns = tick_finished - tick_started;
      sample.callback_wall_ns = callback_finished - callback_started;
      sample.process_cpu_ns = process_finished - process_started;
      sample.thread_cpu_ns = thread_finished - thread_started;
      sample.engine_query_ns = outcome.query_duration_ns;
      sample.status = outcome.status;
      sample.reason = static_cast<int>(outcome.reason);
      sample.framework_allocations =
          allocation_probe::load(
              allocation_probe::Domain::kImmediateFramework) -
          framework_before;
      sample.engine_allocations =
          allocation_probe::load(
              allocation_probe::Domain::kImmediateEngine) -
          engine_before;
      sample.mechanism = subtract(mechanism_after, mechanism_before);
      const int measured =
          state.measured_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
      if (measured >= options.measured_ticks + options.max_extra_ticks &&
          state.measured_lookahead.load(std::memory_order_relaxed) <
              options.measured_lookahead_cycles) {
        fail_campaign(
            state, "lookahead target not reached before extra-tick bound");
      }
    }

    complete_campaign_phase_if_ready(state, options);
    return manyforge::core::Result<void>::Ok();
  };

  RuntimeCollisionMonitor warmup_monitor(
      *native.clock, *native.scheduler,
      RuntimeCollisionMonitor::Config{kMonitorPeriodNs}, immediate_callback,
      [&]() { return state.monitor_continue.load(std::memory_order_acquire); });
  warmup_monitor.start();
  if (!wait_for_phase_end(state, CampaignPhase::kWarmup,
                          options.campaign_timeout_s)) {
    fail_campaign(state, "warmup campaign timed out");
  }
  warmup_monitor.stop(true);
  if (state.phase.load(std::memory_order_acquire) == CampaignPhase::kFailed) {
    lookahead.disarm();
    throw std::runtime_error(campaign_failure_reason(state));
  }

  if (!wait_for_lookahead_idle(lookahead, options.campaign_timeout_s)) {
    lookahead.disarm();
    throw std::runtime_error("lookahead did not quiesce after warmup");
  }

  const auto lookahead_mechanism_baseline = lookahead_accumulator->snapshot();
  {
    std::lock_guard<std::mutex> lock(state.lookahead_mechanism_mutex);
    state.last_lookahead_mechanism = lookahead_mechanism_baseline;
  }
  const auto immediate_mechanism_baseline = immediate_accumulator->snapshot();
  allocation_probe::reset();
  state.monitor_continue.store(true, std::memory_order_release);
  state.phase.store(CampaignPhase::kMeasured, std::memory_order_release);

  RuntimeCollisionMonitor measured_monitor(
      *native.clock, *native.scheduler,
      RuntimeCollisionMonitor::Config{kMonitorPeriodNs}, immediate_callback,
      [&]() { return state.monitor_continue.load(std::memory_order_acquire); });
  measured_monitor.start();
  if (!wait_for_phase_end(state, CampaignPhase::kMeasured,
                          options.campaign_timeout_s)) {
    fail_campaign(state, "measured campaign timed out");
  }
  measured_monitor.stop(true);
  evidence.monitor_metrics = measured_monitor.timing_metrics();
  if (state.phase.load(std::memory_order_acquire) == CampaignPhase::kFailed) {
    lookahead.disarm();
    throw std::runtime_error(campaign_failure_reason(state));
  }
  if (!wait_for_lookahead_idle(lookahead, options.campaign_timeout_s)) {
    lookahead.disarm();
    throw std::runtime_error("lookahead did not quiesce after measurement");
  }
  evidence.lookahead_status = lookahead.status();
  lookahead.disarm();

  evidence.immediate_sample_count =
      state.immediate_sample_count.load(std::memory_order_relaxed);
  evidence.lookahead_sample_count =
      state.lookahead_sample_count.load(std::memory_order_relaxed);
  evidence.immediate_faults =
      state.immediate_faults.load(std::memory_order_relaxed);
  evidence.lookahead_faults =
      state.lookahead_faults.load(std::memory_order_relaxed);
  evidence.exact_32_failures =
      state.exact_32_failures.load(std::memory_order_relaxed);
  evidence.measured_lookahead_requests_issued =
      state.measured_lookahead_requests.load(std::memory_order_relaxed);
  evidence.immediate_samples.assign(
      state.immediate_samples.begin(),
      state.immediate_samples.begin() + evidence.immediate_sample_count);
  evidence.lookahead_samples.assign(
      state.lookahead_samples.begin(),
      state.lookahead_samples.begin() + evidence.lookahead_sample_count);
  evidence.immediate_mechanism = subtract(
      immediate_accumulator->snapshot(), immediate_mechanism_baseline);
  evidence.lookahead_mechanism = subtract(
      lookahead_accumulator->snapshot(), lookahead_mechanism_baseline);

  for (const auto& immediate_sample : evidence.immediate_samples) {
    const bool overlap = std::any_of(
        evidence.lookahead_samples.begin(), evidence.lookahead_samples.end(),
        [&](const LookaheadSample& lookahead_sample) {
          const auto worker_started_at_ns =
              lookahead_sample.requested_at_ns +
              lookahead_sample.start_jitter_ns;
          const auto worker_finished_at_ns =
              worker_started_at_ns +
              lookahead_sample.worker_active_wall_ns;
          return immediate_sample.callback_started_at_ns <
                     worker_finished_at_ns &&
                 immediate_sample.callback_finished_at_ns >
                     worker_started_at_ns;
        });
    if (overlap) {
      ++evidence.overlap_ticks;
    }
  }
  return evidence;
}

void append_cold_sync(std::ostringstream& out,
                      const ColdSyncEvidence& value) {
  out << "{\"evidence_kind\":\""
      << (MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION ? "observation_only"
                                                 : "timing_measurement")
      << "\",\"prepare_status\":\""
      << manyforge::core::runtime_collision_query_prepare_status_name(
             value.status)
      << "\",\"wall_ns\":" << value.wall_ns
      << ",\"process_cpu_ns\":" << value.process_cpu_ns
      << ",\"thread_cpu_ns\":" << value.thread_cpu_ns;
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  out << ",\"immediate_engine_allocations\":"
      << value.immediate_engine_allocations
      << ",\"lookahead_engine_allocations\":"
      << value.lookahead_engine_allocations
      << ",\"scene_build_count_before\":"
      << value.scene_build_count_before
      << ",\"scene_build_count_after\":"
      << value.scene_build_count_after;
#else
  out << ",\"immediate_engine_allocations\":null"
         ",\"lookahead_engine_allocations\":null"
         ",\"scene_build_count_before\":null"
         ",\"scene_build_count_after\":null";
#endif
  out << "}";
}

std::string result_json(const Options& options, const FixtureInputs& fixture,
                        const TrajectoryDTO& trajectory,
                        const RunEvidence& evidence) {
  std::vector<std::int64_t> immediate_wall;
  std::vector<std::int64_t> immediate_callback;
  std::vector<std::int64_t> immediate_process;
  std::vector<std::int64_t> immediate_thread;
  std::vector<std::int64_t> lookahead_cycle;
  std::vector<std::int64_t> lookahead_worker;
  std::vector<std::int64_t> lookahead_process;
  std::vector<std::int64_t> lookahead_jitter;
  std::uint64_t framework_allocations = 0;
  std::uint64_t immediate_engine_allocations = 0;
  int immediate_clear = 0;
  int immediate_collision = 0;
  int immediate_inconclusive = 0;
  for (const auto& sample : evidence.immediate_samples) {
    immediate_wall.push_back(sample.tick_wall_ns);
    immediate_callback.push_back(sample.callback_wall_ns);
    immediate_process.push_back(sample.process_cpu_ns);
    immediate_thread.push_back(sample.thread_cpu_ns);
    framework_allocations += sample.framework_allocations;
    immediate_engine_allocations += sample.engine_allocations;
    if (sample.status == RuntimeCollisionTickStatus::kClear) {
      ++immediate_clear;
    } else if (sample.status == RuntimeCollisionTickStatus::kCollision) {
      ++immediate_collision;
    } else {
      ++immediate_inconclusive;
    }
  }
  int lookahead_clear = 0;
  int lookahead_collision = 0;
  int lookahead_inconclusive = 0;
  std::size_t checked_min = std::numeric_limits<std::size_t>::max();
  std::size_t checked_max = 0;
  std::uint64_t checked_total = 0;
  int process_stamp_missing = 0;
  for (const auto& sample : evidence.lookahead_samples) {
    lookahead_cycle.push_back(sample.cycle_wall_ns);
    lookahead_worker.push_back(sample.worker_active_wall_ns);
    lookahead_process.push_back(sample.process_cpu_ns);
    lookahead_jitter.push_back(sample.start_jitter_ns);
    checked_min = std::min(checked_min, sample.checked_samples);
    checked_max = std::max(checked_max, sample.checked_samples);
    checked_total += sample.checked_samples;
    process_stamp_missing += sample.process_stamp_found ? 0 : 1;
    if (sample.status == RuntimeCollisionLookaheadResultStatus::kClear) {
      ++lookahead_clear;
    } else if (sample.status ==
               RuntimeCollisionLookaheadResultStatus::kCollision) {
      ++lookahead_collision;
    } else {
      ++lookahead_inconclusive;
    }
  }
  if (evidence.lookahead_samples.empty()) {
    checked_min = 0;
  }

  std::ostringstream out;
  out << std::setprecision(17);
  out << "{\"schema_version\":\"manyforge.runtime_collision_schedule_qualification.v2\",";
  out << "\"identity\":{\"evidence_kind\":\""
      << identity_evidence_kind(options)
      << "\",\"measurement_kind\":\""
      << (MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION ? "observation" : "timing")
      << "\",\"run_id\":\"" << json_escape(options.run_id)
      << "\",\"process_instance_id\":\""
      << json_escape(options.process_instance_id)
      << "\",\"source_identity\":";
  append_source_identity(
      out, options.source_head_commit, options.source_head_tree,
      options.source_dirty, options.source_delta_sha256,
      options.source_fingerprint_sha256);
  out << ",\"specs_source_identity\":";
  append_source_identity(
      out, options.specs_head_commit, options.specs_head_tree,
      options.specs_dirty, options.specs_delta_sha256,
      options.specs_fingerprint_sha256);
  out << ",\"specs_contract_identity\":";
  append_specs_contract_identity(
      out, options.specs_head_commit, options.specs_head_tree,
      options.specs_contract_dirty, options.specs_contract_delta_sha256,
      options.specs_contract_fingerprint_sha256);
  out << ",\"image_id\":\"" << json_escape(options.image_id)
      << "\",\"binary_sha256\":\""
      << json_escape(options.binary_sha256)
      << "\",\"compiler\":\"" << json_escape(__VERSION__)
      << "\"},";
  out << "\"provenance\":{\"source_role\":\""
      << json_escape(options.source_role)
      << "\",\"checkout_base_clean\":"
      << json_bool(options.checkout_base_clean)
      << ",\"clean_checkout_manifest_sha256\":";
  append_nullable_json_string(out, options.clean_checkout_manifest_sha256);
  out
      << ",\"qualification_patch_sha256\":";
  append_nullable_json_string(out, options.qualification_patch_sha256);
  out << ",\"observation_patch_sha256\":";
  append_nullable_json_string(out, options.observation_patch_sha256);
  out << ",\"pristine_decision_parity_sha256\":";
  append_nullable_json_string(out, options.pristine_decision_parity_sha256);
  out << ",\"command_argv\":" << options.command_argv_json
      << ",\"runner_command_argv\":" << options.runner_command_argv_json
      << ",\"build_commands_argv\":" << options.build_commands_argv_json
      << ",\"outer_command_argv\":" << options.outer_command_argv_json
      << ",\"runner_environment\":" << options.runner_environment_json
      << ",\"runner_script_sha256\":\""
      << json_escape(options.runner_script_sha256)
      << "\",\"prepare_script_sha256\":";
  append_nullable_json_string(out, options.prepare_script_sha256);
  out << ",\"source_identity_script_sha256\":\""
      << json_escape(options.source_identity_script_sha256) << "\"";
  out << "},";
  out << "\"inputs\":{\"arm\":\"" << arm_name(options.arm)
      << "\",\"fixture_id\":\"step18_runtime_contact_envelope_v2\""
      << ",\"robot_model\":\"ur5_gripper\",\"group\":\"arm\""
      << ",\"geometry_fingerprint_fnv1a64\":\""
      << fixture.geometry_fingerprint
      << "\",\"pair_fingerprint_fnv1a64\":\""
      << fixture.pair_fingerprint
      << "\",\"policy_fingerprint_fnv1a64\":\""
      << fixture.policy_fingerprint
      << "\",\"trajectory_fingerprint_fnv1a64\":\""
      << trajectory_fingerprint(trajectory) << "\",\"world_content_hash\":\""
      << json_escape(fixture.bundle->world->revision.content_hash)
      << "\",\"allowed_contact_pair_count\":"
      << fixture.allowed_pairs->size()
      << ",\"maximum_observed_penetration_m\":"
      << fixture.maximum_observed_penetration_m
      << ",\"bounded_threshold_m\":" << options.bounded_threshold_m
      << ",\"trajectory_waypoint_count\":" << trajectory.waypoints.size()
      << "},";
  out << "\"parity_probe\":{\"evidence_kind\":\"unmeasured_output\""
      << ",\"engine_status_code\":" << fixture.parity_probe_engine_status
      << ",\"decision_code\":" << fixture.parity_probe_decision
      << ",\"normalized_pair_output_fingerprint_fnv1a64\":\""
      << fixture.parity_probe_normalized_pair_output_fingerprint
      << "\",\"normalized_pairs\":[";
  for (std::size_t index = 0;
       index < fixture.parity_probe_normalized_pairs.size(); ++index) {
    if (index != 0) {
      out << ',';
    }
    const auto& pair = fixture.parity_probe_normalized_pairs[index];
    out << "{\"body_a\":\"" << json_escape(pair.body_a)
        << "\",\"body_b\":\"" << json_escape(pair.body_b) << "\"}";
  }
  out << "]},";
  out << "\"config\":{\"monitor_period_ns\":" << kMonitorPeriodNs
      << ",\"immediate_query_budget_ns\":" << kImmediateQueryBudgetNs
      << ",\"lookahead_cycle_budget_ns\":" << kLookaheadCycleBudgetNs
      << ",\"lookahead_horizon_ns\":" << kLookaheadHorizonNs
      << ",\"lookahead_query_budget_ns\":" << kLookaheadQueryBudgetNs
      << ",\"lookahead_max_samples\":" << kLookaheadSamples
      << ",\"lookahead_workers_requested\":" << options.workers
      << ",\"lookahead_workers_effective\":"
      << evidence.lookahead_status.effective_workers
      << ",\"requested_cpu_affinity\":\""
      << json_escape(options.requested_cpu_affinity)
      << "\",\"effective_cpu_affinity\":\""
      << json_escape(effective_cpu_affinity())
      << "\",\"process_scheduler_policy\":" << ::sched_getscheduler(0)
      << ",\"cpu_governor_set\":\""
      << json_escape(options.cpu_governor_set)
      << "\",\"cpu_frequency_driver_set\":\""
      << json_escape(options.cpu_frequency_driver_set)
      << "\",\"kernel_id\":\"" << json_escape(options.host_kernel_id)
      << "\",\"compiler_flags\":\""
      << json_escape(options.compiler_flags)
      << "\",\"build_type\":\"" << json_escape(options.build_type)
      << ",\"warmup_ticks_requested\":" << options.warmup_ticks
      << ",\"warmup_lookahead_cycles_requested\":"
      << options.warmup_lookahead_cycles
      << ",\"measured_ticks_requested\":" << options.measured_ticks
      << ",\"measured_lookahead_cycles_requested\":"
      << options.measured_lookahead_cycles
      << ",\"max_extra_ticks\":" << options.max_extra_ticks
      << ",\"campaign_timeout_s\":" << options.campaign_timeout_s
      << "},";
  out << "\"cold_snapshot_sync\":{\"immediate_context\":";
  append_cold_sync(out, evidence.immediate_cold);
  out << ",\"lookahead_shared_context\":";
  append_cold_sync(out, evidence.lookahead_cold);
  out << "},\"warm_snapshot_reuse\":{\"immediate_context\":";
  append_cold_sync(out, evidence.immediate_warm_reuse);
  out << ",\"lookahead_shared_context\":";
  append_cold_sync(out, evidence.lookahead_warm_reuse);
  out << "},";

  out << "\"immediate\":{\"summary\":{\"evidence_kind\":\""
      << (MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION ? "observation_only"
                                                 : "timing_measurement")
      << "\",\"sample_count\":" << evidence.immediate_sample_count
      << ",\"clear\":" << immediate_clear
      << ",\"collision\":" << immediate_collision
      << ",\"inconclusive\":" << immediate_inconclusive
      << ",\"faults\":" << evidence.immediate_faults
      << ",\"framework_owned_allocations\":";
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  out << framework_allocations << ",\"engine_boundary_allocations\":"
      << immediate_engine_allocations;
#else
  (void)framework_allocations;
  (void)immediate_engine_allocations;
  out << "null,\"engine_boundary_allocations\":null";
#endif
  out << ",\"tick_wall\":";
  append_distribution(out, distribution(std::move(immediate_wall)));
  out << ",\"callback_wall\":";
  append_distribution(out, distribution(std::move(immediate_callback)));
  out << ",\"process_cpu_window\":";
  append_distribution(out, distribution(std::move(immediate_process)));
  out << ",\"thread_cpu\":";
  append_distribution(out, distribution(std::move(immediate_thread)));
  out << ",\"scheduler\":{\"callbacks_started\":"
      << evidence.monitor_metrics.callbacks_started
      << ",\"scheduled_to_start_jitter_max_ns\":"
      << evidence.monitor_metrics.scheduled_to_start_jitter_max_ns
      << ",\"callback_duration_max_ns\":"
      << evidence.monitor_metrics.callback_duration_max_ns
      << ",\"late_periods\":" << evidence.monitor_metrics.late_periods
      << ",\"missed_periods\":" << evidence.monitor_metrics.missed_periods
      << ",\"max_consecutive_late_periods\":"
      << evidence.monitor_metrics.max_consecutive_late_periods
      << "},\"mechanism\":";
  append_mechanism(out, evidence.immediate_mechanism);
  out << "}},";

  out << "\"lookahead\":{\"summary\":{\"evidence_kind\":\""
      << (MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION ? "observation_only"
                                                 : "timing_measurement")
      << "\",\"sample_count\":" << evidence.lookahead_sample_count
      << ",\"clear\":" << lookahead_clear
      << ",\"collision\":" << lookahead_collision
      << ",\"inconclusive_or_other\":" << lookahead_inconclusive
      << ",\"faults\":" << evidence.lookahead_faults
      << ",\"checked_samples_min\":" << checked_min
      << ",\"checked_samples_max\":" << checked_max
      << ",\"checked_samples_total\":" << checked_total
      << ",\"exact_32_failures\":" << evidence.exact_32_failures
      << ",\"process_stamp_missing\":" << process_stamp_missing
      << ",\"cycle_wall\":";
  append_distribution(out, distribution(std::move(lookahead_cycle)));
  out << ",\"worker_active_wall\":";
  append_distribution(out, distribution(std::move(lookahead_worker)));
  out << ",\"process_cpu_window\":";
  append_distribution(out, distribution(std::move(lookahead_process)));
  out << ",\"start_jitter\":";
  append_distribution(out, distribution(std::move(lookahead_jitter)));
  out << ",\"status_counters\":{\"jobs_started_total\":"
      << evidence.lookahead_status.jobs_started_total
      << ",\"jobs_clear_total\":" << evidence.lookahead_status.jobs_clear_total
      << ",\"jobs_collision_total\":"
      << evidence.lookahead_status.jobs_collision_total
      << ",\"jobs_inconclusive_total\":"
      << evidence.lookahead_status.jobs_inconclusive_total
      << ",\"jobs_query_timeout_total\":"
      << evidence.lookahead_status.jobs_query_timeout_total
      << ",\"jobs_cycle_timeout_total\":"
      << evidence.lookahead_status.jobs_cycle_timeout_total
      << ",\"jobs_superseded_total\":"
      << evidence.lookahead_status.jobs_superseded_total
      << ",\"reruns_coalesced_total\":"
      << evidence.lookahead_status.reruns_coalesced_total
      << ",\"clear_permit_try_lock_misses_total\":"
      << evidence.lookahead_status.clear_permit_try_lock_misses_total
      << ",\"stale_generation_results_total\":"
      << evidence.lookahead_status.stale_generation_results_total
      << "},\"mechanism\":";
  append_mechanism(out, evidence.lookahead_mechanism);
  out << "}},";

  out << "\"cross_checks\":{\"semantic_evidence_kind\":\"exact_derivation\""
      << ",\"mechanism_evidence_kind\":\""
      << (MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION ? "direct_counter"
                                                 : "not_measured")
      << "\""
      << ",\"support_contact_active\":true"
      << ",\"exact_32_lookahead\":"
      << json_bool(evidence.exact_32_failures == 0 && checked_min == 32 &&
                   checked_max == 32)
      << ",\"measured_lookahead_requests_issued\":"
      << evidence.measured_lookahead_requests_issued
      << ",\"immediate_lookahead_worker_active_overlap_ticks\":"
      << evidence.overlap_ticks
      << ",\"all_colliding_pairs_declared\":";
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  out << json_bool(evidence.immediate_mechanism
                           .undeclared_colliding_logical_pairs == 0 &&
                   evidence.lookahead_mechanism
                           .undeclared_colliding_logical_pairs == 0);
#else
  out << "null";
#endif
  out
#if MANYFORGE_RUNTIME_SCHEDULE_BASELINE
      << ",\"bounded_semantic_evidence_kind\":\"not_applicable\""
      << ",\"bounded_expected_contacts_under_cap\":null"
      << ",\"bounded_transition_helper_any_allowed_pair_clear_late_appearance_"
         "disappearance_reappearance_final_contact_accepted\":null"
      << ",\"bounded_over_cap_transition_rejected\":null},";
#else
      << ",\"bounded_semantic_evidence_kind\":\"exact_derivation\""
      << ",\"bounded_expected_contacts_under_cap\":"
      << json_bool(options.arm != Arm::kBoundedPenetration ||
                   fixture.maximum_observed_penetration_m <=
                       options.bounded_threshold_m)
      << ",\"bounded_transition_helper_any_allowed_pair_clear_late_appearance_"
         "disappearance_reappearance_final_contact_accepted\":"
      << json_bool(
             fixture.bounded_any_allowed_pair_late_contact_sequence_accepted)
      << ",\"bounded_over_cap_transition_rejected\":"
      << json_bool(fixture.bounded_over_cap_transition_rejected) << "},";
#endif

  out << "\"raw\":{\"immediate_samples\":[";
  for (std::size_t index = 0; index < evidence.immediate_samples.size();
       ++index) {
    if (index != 0) {
      out << ',';
    }
    const auto& sample = evidence.immediate_samples[index];
    out << "{\"sequence\":" << sample.sequence
        << ",\"callback_started_at_ns\":"
        << sample.callback_started_at_ns
        << ",\"callback_finished_at_ns\":"
        << sample.callback_finished_at_ns
        << ",\"started_at_ns\":" << sample.started_at_ns
        << ",\"finished_at_ns\":" << sample.finished_at_ns
        << ",\"tick_wall_ns\":" << sample.tick_wall_ns
        << ",\"callback_wall_ns\":" << sample.callback_wall_ns
        << ",\"process_cpu_ns\":" << sample.process_cpu_ns
        << ",\"thread_cpu_ns\":" << sample.thread_cpu_ns
        << ",\"engine_query_ns\":" << sample.engine_query_ns
        << ",\"status\":\"" << immediate_status_name(sample.status)
        << "\",\"reason_code\":" << sample.reason
        << ",\"framework_allocations\":";
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
    out << sample.framework_allocations << ",\"engine_allocations\":"
        << sample.engine_allocations;
#else
    out << "null,\"engine_allocations\":null";
#endif
    out << "}";
  }
  out << "],\"lookahead_cycles\":[";
  for (std::size_t index = 0; index < evidence.lookahead_samples.size();
       ++index) {
    if (index != 0) {
      out << ',';
    }
    const auto& sample = evidence.lookahead_samples[index];
    out << "{\"requested_at_ns\":" << sample.requested_at_ns
        << ",\"callback_at_ns\":" << sample.callback_at_ns
        << ",\"cycle_wall_ns\":" << sample.cycle_wall_ns
        << ",\"worker_active_wall_ns\":"
        << sample.worker_active_wall_ns
        << ",\"process_cpu_ns\":" << sample.process_cpu_ns
        << ",\"start_jitter_ns\":" << sample.start_jitter_ns
        << ",\"checked_samples\":" << sample.checked_samples
        << ",\"workers_used\":" << sample.workers_used
        << ",\"status\":\"" << lookahead_status_name(sample.status)
        << "\",\"cycle_deadline_exceeded\":"
        << json_bool(sample.cycle_deadline_exceeded)
        << ",\"process_stamp_found\":"
        << json_bool(sample.process_stamp_found) << "}";
  }
  out << "]},";
  out << "\"external_required\":{\"evidence_kind\":\"not_measured\""
      << ",\"controller_progress_latency\":null"
      << ",\"controller_deadline_misses\":null"
      << ",\"focused_browser_viewer\":null"
      << ",\"isaac_ros_process_tree_cpu\":null"
      << ",\"mesh_heavy_ur10_drop_approach\":null"
      << ",\"projection_retry_exhaustion\":null"
      << ",\"reason\":\"requires authorized clean live campaign\"},"
      << "\"promotion_scope\":\"local_runtime_only\"}";
  return out.str();
}

void allocation_attribution_self_test() {
#if MANYFORGE_RUNTIME_SCHEDULE_OBSERVATION
  allocation_probe::reset();
  {
    allocation_probe::Scope framework(
        allocation_probe::Domain::kImmediateFramework);
    auto* one = new std::uint64_t(1);
    {
      allocation_probe::Scope engine(
          allocation_probe::Domain::kImmediateEngine);
      auto* two = new std::uint64_t(2);
      delete two;
    }
    delete one;
  }
  if (allocation_probe::load(
          allocation_probe::Domain::kImmediateFramework) != 1 ||
      allocation_probe::load(allocation_probe::Domain::kImmediateEngine) !=
          1) {
    throw std::runtime_error("allocation attribution self-test failed");
  }
  allocation_probe::reset();
#endif
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    allocation_attribution_self_test();

    if (options.mechanism_replay) {
      std::cout << mechanism_replay_result(options) << '\n';
      return 0;
    }

    ExecutionKernelConfig config;
    config.collision_monitor_period_ns = kMonitorPeriodNs;
    config.runtime_collision_immediate_query_budget_ns =
        kImmediateQueryBudgetNs;
    config.runtime_collision_lookahead_cycle_budget_ns =
        kLookaheadCycleBudgetNs;
    config.runtime_collision_lookahead_horizon_ns = kLookaheadHorizonNs;
    config.runtime_collision_lookahead_max_samples = kLookaheadSamples;
    config.runtime_collision_lookahead_workers = options.workers;
    config.runtime_collision_lookahead_workers_declared = true;
    config.runtime_collision_lookahead_min_waypoints_for_parallel =
        kLookaheadMinParallelWaypoints;
    config.runtime_collision_lookahead_query_budget_ns =
        kLookaheadQueryBudgetNs;
    config.validation_collision_workers = 1;
    config.validation_collision_workers_declared = true;
    auto native = create_native_kernel("ur5_gripper", "arm", config);
    if (!native || !native->scene_authority || !native->clock ||
        !native->scheduler) {
      throw std::runtime_error("failed to create native qualification bundle");
    }
    const auto fixture = configure_fixture(*native, options);
    const auto trajectory = static_lookahead_trajectory(fixture.state);
    const auto selection = manyforge::core::
        select_runtime_collision_lookahead_samples(
            trajectory, 0, kLookaheadHorizonNs, kLookaheadSamples);
    if (selection.trajectory.waypoints.size() != kLookaheadSamples ||
        selection.reaches_trajectory_end ||
        selection.termination !=
            manyforge::core::RuntimeCollisionLookaheadTermination::
                kTimeHorizon) {
      throw std::runtime_error(
          "fixture does not select an exact 32-sample non-terminal prefix");
    }
    const auto evidence =
        run_campaign(*native, fixture, trajectory, options);
    std::cout << result_json(options, fixture, trajectory, evidence) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "runtime collision schedule qualification failed: "
              << error.what() << '\n';
    return 1;
  }
}
