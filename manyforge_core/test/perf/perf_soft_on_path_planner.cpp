#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::size_t> g_allocation_count{0};

[[gnu::noinline]] void* allocate_bytes(std::size_t size) {
  return std::malloc(size == 0 ? 1 : size);
}

[[gnu::noinline]] void free_bytes(void* ptr) {
  std::free(ptr);
}

}  // namespace

void* operator new(std::size_t size) {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  void* ptr = allocate_bytes(size);
  if (ptr == nullptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

void* operator new[](std::size_t size) {
  return operator new(size);
}

void operator delete(void* ptr) noexcept {
  free_bytes(ptr);
}

void operator delete[](void* ptr) noexcept {
  free_bytes(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
  free_bytes(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
  free_bytes(ptr);
}

namespace {

using manyforge::core::AngularAccelerationDTO;
using manyforge::core::MotionInterruptStopMode;
using manyforge::core::MotionInterruptStopProfileDTO;
using manyforge::core::RobotStateDTO;
using manyforge::core::SoftOnPathInitialStateDTO;
using manyforge::core::SoftOnPathPlannerConfig;
using manyforge::core::SoftOnPathStateSource;
using manyforge::core::TrajectoryDTO;
using manyforge::core::TrajectorySampleResult;
using manyforge::core::TrajectoryWaypointDTO;

constexpr std::size_t kJointCount = 6;

std::size_t read_size_env(const char* name, std::size_t fallback) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return fallback;
  }
  try {
    const auto parsed = std::stoull(raw);
    return parsed == 0 ? fallback : static_cast<std::size_t>(parsed);
  } catch (...) {
    return fallback;
  }
}

std::optional<std::size_t> read_optional_size_env(const char* name) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return std::nullopt;
  }
  try {
    return static_cast<std::size_t>(std::stoull(raw));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> read_double_env(const char* name) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return std::nullopt;
  }
  try {
    const double parsed = std::stod(raw);
    if (std::isfinite(parsed) && parsed > 0.0) {
      return parsed;
    }
  } catch (...) {
  }
  return std::nullopt;
}

std::string format_double(double value) {
  std::ostringstream out;
  out << std::setprecision(12) << value;
  return out.str();
}

struct PathPoint {
  std::vector<double> q;
  std::vector<double> v;
  std::vector<double> a;
};

PathPoint model_point(double t) {
  PathPoint point;
  point.q.resize(kJointCount, 0.0);
  point.v.resize(kJointCount, 0.0);
  point.a.resize(kJointCount, 0.0);
  for (std::size_t j = 0; j < kJointCount; ++j) {
    const double phase = 0.35 * static_cast<double>(j);
    const double slope = 0.035 + 0.004 * static_cast<double>(j);
    const double amplitude = 0.018 + 0.002 * static_cast<double>(j);
    const double omega = 0.45 + 0.03 * static_cast<double>(j);
    point.q[j] = -0.2 + 0.08 * static_cast<double>(j) + slope * t +
                 amplitude * std::sin(omega * t + phase);
    point.v[j] = slope + amplitude * omega * std::cos(omega * t + phase);
    point.a[j] = -amplitude * omega * omega * std::sin(omega * t + phase);
  }
  return point;
}

TrajectoryDTO make_reference_trajectory() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"shoulder_pan_joint",
                            "shoulder_lift_joint",
                            "elbow_joint",
                            "wrist_1_joint",
                            "wrist_2_joint",
                            "wrist_3_joint"};
  constexpr double kDurationS = 8.0;
  constexpr std::size_t kWaypoints = 241;
  trajectory.waypoints.reserve(kWaypoints);
  for (std::size_t i = 0; i < kWaypoints; ++i) {
    const double t = kDurationS * static_cast<double>(i) /
                     static_cast<double>(kWaypoints - 1);
    const auto point = model_point(t);
    trajectory.waypoints.push_back(
        TrajectoryWaypointDTO{t, point.q, point.v, point.a});
  }
  return trajectory;
}

MotionInterruptStopProfileDTO make_profile(const TrajectoryDTO& trajectory) {
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "phase_4_5g_perf_soft_on_path";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 0.25;
  profile.recoverable_substitution_mode =
      MotionInterruptStopMode::kSpringBackHold;
  for (const auto& joint : trajectory.joint_names) {
    profile.per_joint_max_deceleration[joint] =
        AngularAccelerationDTO{40.0};
  }
  return profile;
}

SoftOnPathPlannerConfig make_config(const TrajectoryDTO& trajectory) {
  SoftOnPathPlannerConfig config;
  config.stop_traj_delta_s = 0.02;
  config.sample_period_s = 0.01;
  config.allow_model_velocity_fallback = true;
  config.max_initial_path_deviation_rad = 0.02;
  config.max_initial_velocity_deviation_rad_s = 0.08;
  for (const auto& joint : trajectory.joint_names) {
    config.per_joint_max_jerk_rad_s3[joint] = 2000.0;
  }
  return config;
}

SoftOnPathInitialStateDTO make_initial_state(
    const TrajectoryDTO& trajectory,
    std::size_t iteration) {
  const std::size_t first_index = 8;
  const std::size_t last_index = trajectory.waypoints.size() - 40;
  const std::size_t span = last_index - first_index;
  const std::size_t index = first_index + (iteration % span);
  const auto& waypoint = trajectory.waypoints[index];

  RobotStateDTO state;
  state.joint_names = trajectory.joint_names;
  state.positions_rad = waypoint.positions_rad;

  SoftOnPathInitialStateDTO initial;
  initial.state = std::move(state);
  initial.trajectory_time_from_start_s = waypoint.time_from_start_s;
  initial.position_source = SoftOnPathStateSource::kMeasuredFeedback;
  initial.velocity_source = SoftOnPathStateSource::kModelDerived;
  initial.runtime_velocity_scale_at_trip =
      0.25 + 0.25 * static_cast<double>(iteration % 4);
  initial.sample_age_ns = 1'000'000;
  return initial;
}

struct Percentiles {
  double p50_us = 0.0;
  double p95_us = 0.0;
  double p99_us = 0.0;
  double p999_us = 0.0;
  double max_us = 0.0;
  double mean_us = 0.0;
};

struct AllocationProbeResult {
  std::size_t iterations = 0;
  std::size_t allocations = 0;
  double allocations_per_iteration = 0.0;
  double total_us = 0.0;
  double mean_us = 0.0;
  std::size_t checksum = 0;
};

double percentile(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double position = q * static_cast<double>(sorted.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  if (lower == upper) {
    return sorted[lower];
  }
  const double alpha = position - static_cast<double>(lower);
  return sorted[lower] * (1.0 - alpha) + sorted[upper] * alpha;
}

Percentiles summarize(std::vector<double> samples_us) {
  std::sort(samples_us.begin(), samples_us.end());
  Percentiles p;
  p.p50_us = percentile(samples_us, 0.50);
  p.p95_us = percentile(samples_us, 0.95);
  p.p99_us = percentile(samples_us, 0.99);
  p.p999_us = percentile(samples_us, 0.999);
  p.max_us = samples_us.empty() ? 0.0 : samples_us.back();
  p.mean_us = samples_us.empty()
                  ? 0.0
                  : std::accumulate(samples_us.begin(), samples_us.end(), 0.0) /
                        static_cast<double>(samples_us.size());
  return p;
}

std::string percentiles_json(const Percentiles& p) {
  std::ostringstream out;
  out << "\"p50_us\":" << format_double(p.p50_us)
      << ",\"p95_us\":" << format_double(p.p95_us)
      << ",\"p99_us\":" << format_double(p.p99_us)
      << ",\"p999_us\":" << format_double(p.p999_us)
      << ",\"max_us\":" << format_double(p.max_us)
      << ",\"mean_us\":" << format_double(p.mean_us);
  return out.str();
}

std::string allocation_probe_json(const AllocationProbeResult& p) {
  std::ostringstream out;
  out << "\"iterations\":" << p.iterations
      << ",\"allocations\":" << p.allocations
      << ",\"allocations_per_iteration\":"
      << format_double(p.allocations_per_iteration)
      << ",\"total_us\":" << format_double(p.total_us)
      << ",\"mean_us\":" << format_double(p.mean_us)
      << ",\"checksum\":" << p.checksum;
  return out.str();
}

template <typename Fn>
AllocationProbeResult measure_allocations(std::size_t iterations, Fn&& fn) {
  g_allocation_count.store(0, std::memory_order_relaxed);
  g_count_allocations.store(true, std::memory_order_release);
  const auto start = std::chrono::steady_clock::now();
  const std::size_t checksum = fn();
  const auto end = std::chrono::steady_clock::now();
  g_count_allocations.store(false, std::memory_order_release);

  AllocationProbeResult result;
  result.iterations = iterations;
  result.allocations = g_allocation_count.load(std::memory_order_relaxed);
  result.allocations_per_iteration =
      iterations == 0 ? 0.0
                      : static_cast<double>(result.allocations) /
                            static_cast<double>(iterations);
  result.total_us = std::chrono::duration<double, std::micro>(end - start).count();
  result.mean_us = iterations == 0 ? 0.0
                                   : result.total_us /
                                         static_cast<double>(iterations);
  result.checksum = checksum;
  return result;
}

AllocationProbeResult measure_prevalidated_sampler_allocations(
    const TrajectoryDTO& trajectory, std::size_t iterations) {
  auto valid = manyforge::core::validate_trajectory_for_sampling(trajectory);
  if (!valid.ok()) {
    std::cerr << "reference trajectory failed sampler validation: "
              << valid.error().code << "\n";
    std::exit(2);
  }
  TrajectorySampleResult reused;
  auto warm = manyforge::core::sample_trajectory_prevalidated(
      trajectory, 0.0, reused);
  if (!warm.ok()) {
    std::cerr << "prevalidated sampler warmup failed: "
              << warm.error().code << "\n";
    std::exit(2);
  }
  const double duration_s = trajectory.waypoints.back().time_from_start_s;
  return measure_allocations(iterations, [&]() {
    std::size_t checksum = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
      const double t =
          duration_s * static_cast<double>((i * 37) %
                                           (trajectory.waypoints.size() - 1)) /
          static_cast<double>(trajectory.waypoints.size() - 1);
      auto sampled = manyforge::core::sample_trajectory_prevalidated(
          trajectory, t, reused);
      if (!sampled.ok()) {
        std::cerr << "prevalidated sampler failed at iteration " << i
                  << ": " << sampled.error().code << "\n";
        std::exit(2);
      }
      checksum += reused.positions_rad.size();
      checksum += reused.velocities_rad_s.size();
      checksum += reused.accelerations_rad_s2.size();
    }
    return checksum;
  });
}

AllocationProbeResult measure_classic_sampler_allocations(
    const TrajectoryDTO& trajectory, std::size_t iterations) {
  const double duration_s = trajectory.waypoints.back().time_from_start_s;
  return measure_allocations(iterations, [&]() {
    std::size_t checksum = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
      const double t =
          duration_s * static_cast<double>((i * 37) %
                                           (trajectory.waypoints.size() - 1)) /
          static_cast<double>(trajectory.waypoints.size() - 1);
      auto sampled = manyforge::core::sample_trajectory(trajectory, t);
      if (!sampled.ok()) {
        std::cerr << "classic sampler failed at iteration " << i
                  << ": " << sampled.error().code << "\n";
        std::exit(2);
      }
      checksum += sampled.value().positions_rad.size();
      checksum += sampled.value().velocities_rad_s.size();
      checksum += sampled.value().accelerations_rad_s2.size();
    }
    return checksum;
  });
}

AllocationProbeResult measure_shared_reference_snapshot_allocations(
    const TrajectoryDTO& trajectory, std::size_t iterations) {
  auto shared = std::make_shared<const TrajectoryDTO>(trajectory);
  std::mutex mutex;
  std::shared_ptr<const TrajectoryDTO> active = shared;
  std::shared_ptr<const TrajectoryDTO> snapshot;
  return measure_allocations(iterations, [&]() {
    std::size_t checksum = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot = active;
      }
      checksum += snapshot->waypoints.size();
      checksum += snapshot->joint_names.size();
    }
    return checksum;
  });
}

AllocationProbeResult measure_legacy_deep_copy_snapshot_allocations(
    const TrajectoryDTO& trajectory, std::size_t iterations) {
  std::mutex mutex;
  TrajectoryDTO active = trajectory;
  return measure_allocations(iterations, [&]() {
    std::size_t checksum = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
      TrajectoryDTO snapshot;
      {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot = active;
      }
      checksum += snapshot.waypoints.size();
      checksum += snapshot.joint_names.size();
    }
    return checksum;
  });
}

AllocationProbeResult measure_path_deviation_allocations(
    const TrajectoryDTO& trajectory, std::size_t iterations) {
  RobotStateDTO observed;
  observed.joint_names = trajectory.joint_names;
  observed.positions_rad = trajectory.waypoints[trajectory.waypoints.size() / 2]
                               .positions_rad;
  return measure_allocations(iterations, [&]() {
    std::size_t checksum = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
      auto measured =
          manyforge::core::measure_soft_on_path_path_deviation(trajectory,
                                                               observed);
      if (!measured.ok()) {
        std::cerr << "path deviation failed at iteration " << i << ": "
                  << measured.error().code << "\n";
        std::exit(2);
      }
      checksum += static_cast<std::size_t>(measured.value() * 1'000'000.0);
      checksum += observed.positions_rad.size();
    }
    return checksum;
  });
}

}  // namespace

int main() {
  const auto trajectory = make_reference_trajectory();
  const auto profile = make_profile(trajectory);
  const auto config = make_config(trajectory);
  const std::size_t plan_iterations =
      read_size_env("MANYFORGE_SOFT_ON_PATH_PLAN_ITERATIONS", 5000);
  const std::size_t envelope_iterations =
      read_size_env("MANYFORGE_SOFT_ON_PATH_ENVELOPE_ITERATIONS", 20);
  const auto plan_p999_budget_us =
      read_double_env("MANYFORGE_SOFT_ON_PATH_PLAN_P999_BUDGET_US");
  const auto envelope_p99_budget_us =
      read_double_env("MANYFORGE_SOFT_ON_PATH_ENVELOPE_P99_BUDGET_US");
  const std::size_t allocation_iterations =
      read_size_env("MANYFORGE_SOFT_ON_PATH_ALLOCATION_ITERATIONS", 1000);
  const auto prevalidated_allocation_budget =
      read_optional_size_env("MANYFORGE_SOFT_ON_PATH_PREVALIDATED_ALLOC_BUDGET");
  const auto shared_reference_allocation_budget =
      read_optional_size_env("MANYFORGE_SOFT_ON_PATH_SHARED_REFERENCE_ALLOC_BUDGET");

  std::vector<double> plan_samples_us;
  plan_samples_us.reserve(plan_iterations);
  for (std::size_t i = 0; i < plan_iterations; ++i) {
    const auto initial = make_initial_state(trajectory, i);
    const auto start = std::chrono::steady_clock::now();
    const auto result = manyforge::core::plan_soft_on_path_stop(
        trajectory, profile, initial, config);
    const auto end = std::chrono::steady_clock::now();
    if (!result.ok()) {
      std::cerr << "soft_on_path planner rejected sample " << i << ": "
                << result.error().message << "\n";
      return 2;
    }
    plan_samples_us.push_back(
        std::chrono::duration<double, std::micro>(end - start).count());
  }
  const auto plan_percentiles = summarize(std::move(plan_samples_us));

  std::vector<double> envelope_samples_us;
  envelope_samples_us.reserve(envelope_iterations);
  std::size_t envelope_checked_samples = 0;
  std::size_t envelope_natural_completion_samples = 0;
  std::size_t envelope_rejections = 0;
  for (std::size_t i = 0; i < envelope_iterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const auto result =
        manyforge::core::validate_soft_on_path_feasibility_envelope(
            trajectory, profile, config);
    const auto end = std::chrono::steady_clock::now();
    if (result.ok()) {
      envelope_checked_samples = result.value().checked_samples;
      envelope_natural_completion_samples =
          result.value().natural_completion_samples;
    } else {
      const auto& error = result.error();
      const auto reason_it = error.details.find("reason");
      const auto fallback_it = error.details.find("recommended_fallback");
      const bool qualified_fallback_gap =
          reason_it != error.details.end() &&
          reason_it->second == "soft_on_path_feasibility_envelope_infeasible" &&
          fallback_it != error.details.end() &&
          fallback_it->second == "spring_back_hold";
      if (!qualified_fallback_gap) {
        std::cerr << "soft_on_path feasibility envelope rejected iteration "
                  << i << ": " << error.message << "\n";
        return 2;
      }
      ++envelope_rejections;
      const auto checked_it = error.details.find("checked_samples");
      if (checked_it != error.details.end()) {
        try {
          envelope_checked_samples = std::max(
              envelope_checked_samples,
              static_cast<std::size_t>(std::stoull(checked_it->second)));
        } catch (...) {
        }
      }
    }
    envelope_samples_us.push_back(
        std::chrono::duration<double, std::micro>(end - start).count());
  }
  const auto envelope_percentiles = summarize(std::move(envelope_samples_us));

  const auto prevalidated_sampler_allocations =
      measure_prevalidated_sampler_allocations(trajectory, allocation_iterations);
  const auto classic_sampler_allocations =
      measure_classic_sampler_allocations(trajectory, allocation_iterations);
  const auto shared_reference_allocations =
      measure_shared_reference_snapshot_allocations(trajectory,
                                                    allocation_iterations);
  const auto legacy_deep_copy_allocations =
      measure_legacy_deep_copy_snapshot_allocations(trajectory,
                                                    allocation_iterations);
  const auto path_deviation_allocations =
      measure_path_deviation_allocations(trajectory, allocation_iterations);

  bool budget_pass = true;
  if (plan_p999_budget_us.has_value() &&
      plan_percentiles.p999_us > *plan_p999_budget_us) {
    budget_pass = false;
  }
  if (envelope_p99_budget_us.has_value() &&
      envelope_percentiles.p99_us > *envelope_p99_budget_us) {
    budget_pass = false;
  }
  if (prevalidated_allocation_budget.has_value() &&
      prevalidated_sampler_allocations.allocations >
          *prevalidated_allocation_budget) {
    budget_pass = false;
  }
  if (shared_reference_allocation_budget.has_value() &&
      shared_reference_allocations.allocations >
          *shared_reference_allocation_budget) {
    budget_pass = false;
  }

  std::ostringstream out;
  out << "{"
      << "\"scenario\":\"phase_4_5g_soft_on_path_planner_latency\","
      << "\"iterations\":" << plan_iterations << ","
      << "\"envelope_iterations\":" << envelope_iterations << ","
      << "\"trajectory_waypoints\":" << trajectory.waypoints.size() << ","
      << "\"joint_count\":" << trajectory.joint_names.size() << ","
      << "\"deceleration_time_s\":"
      << format_double(*profile.deceleration_time_s) << ","
      << "\"stop_traj_delta_s\":"
      << format_double(config.stop_traj_delta_s) << ","
      << "\"sample_period_s\":" << format_double(config.sample_period_s)
      << ",\"trip_time_plan\":{" << percentiles_json(plan_percentiles) << "},"
      << "\"arm_time_feasibility_envelope\":{\"checked_samples\":"
      << envelope_checked_samples << ",\"natural_completion_samples\":"
      << envelope_natural_completion_samples
      << ",\"qualified_fallback_rejections\":"
      << envelope_rejections << "," << percentiles_json(envelope_percentiles)
      << "},\"allocation_counts\":{\"prevalidated_sampling\":{"
      << allocation_probe_json(prevalidated_sampler_allocations)
      << "},\"classic_sampling\":{"
      << allocation_probe_json(classic_sampler_allocations)
      << "},\"active_reference_snapshot_shared_ptr\":{"
      << allocation_probe_json(shared_reference_allocations)
      << "},\"legacy_deep_copy_snapshot_baseline\":{"
      << allocation_probe_json(legacy_deep_copy_allocations)
      << "},\"path_deviation_measurement\":{"
      << allocation_probe_json(path_deviation_allocations)
      << "}},\"budgets\":{\"plan_p999_us\":";
  if (plan_p999_budget_us.has_value()) {
    out << format_double(*plan_p999_budget_us);
  } else {
    out << "null";
  }
  out << ",\"envelope_p99_us\":";
  if (envelope_p99_budget_us.has_value()) {
    out << format_double(*envelope_p99_budget_us);
  } else {
    out << "null";
  }
  out << ",\"prevalidated_sampling_allocations\":";
  if (prevalidated_allocation_budget.has_value()) {
    out << *prevalidated_allocation_budget;
  } else {
    out << "null";
  }
  out << ",\"active_reference_snapshot_shared_ptr_allocations\":";
  if (shared_reference_allocation_budget.has_value()) {
    out << *shared_reference_allocation_budget;
  } else {
    out << "null";
  }
  out << ",\"passed\":" << (budget_pass ? "true" : "false") << "}}";

  const std::string json = out.str();
  if (const char* out_path = std::getenv("MANYFORGE_SOFT_ON_PATH_QUAL_JSON_OUT");
      out_path != nullptr && *out_path != '\0') {
    std::ofstream f(out_path);
    if (!f) {
      std::cerr << "failed to open MANYFORGE_SOFT_ON_PATH_QUAL_JSON_OUT="
                << out_path << "\n";
      return 2;
    }
    f << json << "\n";
  }

  std::cout << "MANYFORGE_QUALIFICATION_EVIDENCE " << json << "\n";
  return budget_pass ? 0 : 3;
}
