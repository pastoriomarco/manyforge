#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/utsname.h>
#include <vector>

#include <Eigen/Core>
#include <roboplan/core/collision_context.hpp>
#include <roboplan/core/scene.hpp>
#include <roboplan/core/types.hpp>
#include <roboplan_example_models/resources.hpp>
#include <roboplan_rrt/rrt.hpp>

namespace allocation_probe {

std::atomic<bool> enabled{false};
std::atomic<std::size_t> count{0U};
std::atomic<std::size_t> bytes{0U};

void record(std::size_t size) noexcept {
  if (enabled.load(std::memory_order_relaxed)) {
    count.fetch_add(1U, std::memory_order_relaxed);
    bytes.fetch_add(size, std::memory_order_relaxed);
  }
}

void* allocate(std::size_t size) {
  if (void* pointer = std::malloc(size == 0U ? 1U : size)) {
    record(size);
    return pointer;
  }
  throw std::bad_alloc();
}

void* allocate_aligned(std::size_t size, std::size_t alignment) {
  void* pointer = nullptr;
  if (const int error =
          posix_memalign(&pointer, alignment, size == 0U ? 1U : size);
      error != 0) {
    if (error == ENOMEM) {
      throw std::bad_alloc();
    }
    throw std::runtime_error("posix_memalign rejected the requested alignment");
  }
  record(size);
  return pointer;
}

}  // namespace allocation_probe

void* operator new(std::size_t size) { return allocation_probe::allocate(size); }
void* operator new[](std::size_t size) { return allocation_probe::allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocation_probe::allocate_aligned(
      size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocation_probe::allocate_aligned(
      size, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer, std::align_val_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
  std::free(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t,
                       std::align_val_t) noexcept {
  std::free(pointer);
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kWarmups = 2U;
constexpr std::size_t kIterations = 20U;
constexpr std::size_t kBatches = 5U;

struct Sample {
  double latency_ms = 0.0;
  std::size_t allocations = 0U;
  std::size_t allocated_bytes = 0U;
  std::size_t waypoints = 0U;
};

struct Summary {
  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double mean_ms = 0.0;
  double throughput_hz = 0.0;
  double allocations_per_plan = 0.0;
  double allocated_bytes_per_plan = 0.0;
  std::size_t waypoints = 0U;
};

class AllocationWindow {
 public:
  AllocationWindow() {
    allocation_probe::count.store(0U, std::memory_order_relaxed);
    allocation_probe::bytes.store(0U, std::memory_order_relaxed);
    allocation_probe::enabled.store(true, std::memory_order_release);
  }

  AllocationWindow(const AllocationWindow&) = delete;
  AllocationWindow& operator=(const AllocationWindow&) = delete;

  ~AllocationWindow() {
    allocation_probe::enabled.store(false, std::memory_order_release);
  }
};

double percentile(std::vector<double> values, double fraction) {
  std::sort(values.begin(), values.end());
  const auto index = std::min(
      values.size() - 1U,
      static_cast<std::size_t>(std::ceil(fraction * values.size())) - 1U);
  return values.at(index);
}

Summary summarize(const std::vector<Sample>& samples) {
  if (samples.empty()) {
    throw std::runtime_error("A/B sample set is empty");
  }
  std::vector<double> latencies;
  latencies.reserve(samples.size());
  double total_ms = 0.0;
  std::size_t total_allocations = 0U;
  std::size_t total_bytes = 0U;
  const auto waypoints = samples.front().waypoints;
  for (const auto& sample : samples) {
    if (!std::isfinite(sample.latency_ms) || sample.latency_ms <= 0.0 ||
        sample.waypoints != waypoints || waypoints < 2U) {
      throw std::runtime_error("A/B sample invariant failed");
    }
    latencies.push_back(sample.latency_ms);
    total_ms += sample.latency_ms;
    total_allocations += sample.allocations;
    total_bytes += sample.allocated_bytes;
  }
  return Summary{
      percentile(latencies, 0.50), percentile(latencies, 0.95),
      total_ms / static_cast<double>(samples.size()),
      static_cast<double>(samples.size()) / (total_ms / 1000.0),
      static_cast<double>(total_allocations) /
          static_cast<double>(samples.size()),
      static_cast<double>(total_bytes) / static_cast<double>(samples.size()),
      waypoints};
}

struct Fixture {
  std::shared_ptr<roboplan::Scene> scene;
  roboplan::RRT planner;
  roboplan::CollisionQueryView serialized_query;
  roboplan::JointConfiguration start;
  roboplan::JointConfiguration goal;

  Fixture()
      : scene(make_scene()),
        planner(scene, make_options()),
        serialized_query(
            roboplan::CollisionQueryView::forSerializedScene(scene)),
        start(make_configuration(
            {2.261477656358459, 1.9570617913216783,
             -1.156210802275467, 2.0229644353394325,
             -0.8713978455862086, -1.3594747944952112})),
        goal(make_configuration(
            {2.959756700843902, -1.6588986483183334,
             -1.2938684506809712, -0.4132159894019849,
             -2.76715775824764, 0.7740437209968403})) {}

 private:
  static std::shared_ptr<roboplan::Scene> make_scene() {
    const auto models = roboplan::example_models::get_package_models_dir();
    return std::make_shared<roboplan::Scene>(
        "e1_patch0007_ab", models / "ur_robot_model/ur5_gripper.urdf",
        models / "ur_robot_model/ur5_gripper.srdf",
        std::vector<std::filesystem::path>{
            roboplan::example_models::get_package_share_dir()});
  }

  static roboplan::RRTOptions make_options() {
    roboplan::RRTOptions options;
    options.group_name = "arm";
    options.max_nodes = 1000;
    options.max_connection_distance = 1.0;
    options.collision_check_step_size = 0.05;
    options.max_planning_time = 0.0;
    options.rrt_connect = false;
    options.rrt_star = false;
    options.fast_return = true;
    return options;
  }

  static roboplan::JointConfiguration make_configuration(
      std::initializer_list<double> values) {
    const std::vector<std::string> names{
        "shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint",
        "wrist_1_joint",      "wrist_2_joint",       "wrist_3_joint"};
    const std::vector<double> positions(values);
    return roboplan::JointConfiguration{
        names,
        Eigen::Map<const Eigen::VectorXd>(
            positions.data(), static_cast<Eigen::Index>(positions.size())),
        {}, {}};
  }
};

Sample measure(Fixture& fixture, bool reuse_serialized_scene_scratch) {
  fixture.planner.setRngSeed(1234U);
  const auto started = Clock::now();
  allocation_probe::count.store(0U, std::memory_order_relaxed);
  allocation_probe::bytes.store(0U, std::memory_order_relaxed);
  std::size_t waypoints = 0U;
  {
    AllocationWindow window;
    const auto result = reuse_serialized_scene_scratch
                            ? fixture.planner.plan(
                                  fixture.start, fixture.goal,
                                  fixture.serialized_query)
                            : fixture.planner.plan(fixture.start, fixture.goal);
    if (!result.has_value()) {
      throw std::runtime_error("RRT A/B operation failed: " + result.error());
    }
    waypoints = result->positions.size();
  }
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - started).count();
  return Sample{elapsed_ms,
                allocation_probe::count.load(std::memory_order_relaxed),
                allocation_probe::bytes.load(std::memory_order_relaxed),
                waypoints};
}

void warm(Fixture& fixture) {
  for (std::size_t index = 0; index < kWarmups; ++index) {
    static_cast<void>(measure(fixture, false));
    static_cast<void>(measure(fixture, true));
  }
}

std::string architecture() {
  utsname identity{};
  if (uname(&identity) != 0) {
    throw std::runtime_error("uname failed");
  }
  return identity.machine;
}

std::vector<int> affinity() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0) {
    throw std::runtime_error("sched_getaffinity failed");
  }
  std::vector<int> result;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &set)) {
      result.push_back(cpu);
    }
  }
  return result;
}

std::string required_environment(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || std::string_view(value) != "1") {
    throw std::runtime_error(std::string(name) + " must be exactly 1");
  }
  return value;
}

double percent_delta(double candidate, double control) {
  return ((candidate / control) - 1.0) * 100.0;
}

template <typename Selector>
double batch_median(const std::vector<Summary>& summaries,
                    Selector selector) {
  std::vector<double> values;
  values.reserve(summaries.size());
  std::transform(summaries.begin(), summaries.end(),
                 std::back_inserter(values), selector);
  return percentile(std::move(values), 0.50);
}

Summary median_summary(const std::vector<Summary>& summaries) {
  if (summaries.empty()) {
    throw std::runtime_error("A/B batch summary set is empty");
  }
  const auto waypoints = summaries.front().waypoints;
  if (!std::all_of(summaries.begin(), summaries.end(),
                   [waypoints](const auto& value) {
                     return value.waypoints == waypoints;
                   })) {
    throw std::runtime_error("A/B waypoint result changed between batches");
  }
  return Summary{
      batch_median(summaries,
                   [](const auto& value) { return value.p50_ms; }),
      batch_median(summaries,
                   [](const auto& value) { return value.p95_ms; }),
      batch_median(summaries,
                   [](const auto& value) { return value.mean_ms; }),
      batch_median(summaries,
                   [](const auto& value) { return value.throughput_hz; }),
      batch_median(summaries, [](const auto& value) {
        return value.allocations_per_plan;
      }),
      batch_median(summaries, [](const auto& value) {
        return value.allocated_bytes_per_plan;
      }),
      waypoints};
}

void write_summary(const Summary& summary) {
  std::cout << "{\"p50_ms\":" << summary.p50_ms
            << ",\"p95_ms\":" << summary.p95_ms
            << ",\"mean_ms\":" << summary.mean_ms
            << ",\"throughput_hz\":" << summary.throughput_hz
            << ",\"operator_new_allocations_per_plan\":"
            << summary.allocations_per_plan
            << ",\"operator_new_bytes_per_plan\":"
            << summary.allocated_bytes_per_plan << ",\"waypoints\":"
            << summary.waypoints << "}";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "usage: patch0007_ab SOURCE_COMMIT SOURCE_TREE PATCH_SET_SHA256 "
                 "BUILD_IDENTITY_SHA256\n";
    return 2;
  }
  try {
    for (const char* name : {"OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS",
                             "MKL_NUM_THREADS", "VECLIB_MAXIMUM_THREADS",
                             "NUMEXPR_NUM_THREADS"}) {
      static_cast<void>(required_environment(name));
    }
    Fixture fixture;
    warm(fixture);
    struct Batch {
      Summary ordinary;
      Summary reused;
      const char* order;
    };
    std::vector<Batch> batches;
    batches.reserve(kBatches);
    for (std::size_t batch = 0; batch < kBatches; ++batch) {
      std::vector<Sample> ordinary;
      std::vector<Sample> reused;
      ordinary.reserve(kIterations);
      reused.reserve(kIterations);
      const bool ordinary_first = batch % 2U == 0U;
      for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        if (ordinary_first) {
          ordinary.push_back(measure(fixture, false));
          reused.push_back(measure(fixture, true));
        } else {
          reused.push_back(measure(fixture, true));
          ordinary.push_back(measure(fixture, false));
        }
      }
      batches.push_back(Batch{summarize(ordinary), summarize(reused),
                              ordinary_first ? "ordinary_then_reused"
                                             : "reused_then_ordinary"});
    }
    std::vector<Summary> ordinary_summaries;
    std::vector<Summary> reused_summaries;
    ordinary_summaries.reserve(batches.size());
    reused_summaries.reserve(batches.size());
    for (const auto& batch : batches) {
      ordinary_summaries.push_back(batch.ordinary);
      reused_summaries.push_back(batch.reused);
    }
    const auto ordinary_median = median_summary(ordinary_summaries);
    const auto reused_median = median_summary(reused_summaries);

    std::cout << std::setprecision(17)
              << "{\"schema\":\"manyforge.e1.patch0007_collision_reuse_ab.v1\""
              << ",\"source_identity\":{\"commit\":\"" << argv[1]
              << "\",\"tree\":\"" << argv[2]
              << "\",\"patch_set_sha256\":\"" << argv[3]
              << "\",\"build_identity_sha256\":\"" << argv[4] << "\"}"
              << ",\"runtime\":{\"architecture\":\"" << architecture()
              << "\",\"cpu_affinity\":[";
    const auto cpus = affinity();
    for (std::size_t index = 0; index < cpus.size(); ++index) {
      std::cout << (index == 0U ? "" : ",") << cpus[index];
    }
    std::cout << "],\"thread_policy\":{\"OMP_NUM_THREADS\":\"1\","
                 "\"OPENBLAS_NUM_THREADS\":\"1\",\"MKL_NUM_THREADS\":\"1\","
                 "\"VECLIB_MAXIMUM_THREADS\":\"1\","
                 "\"NUMEXPR_NUM_THREADS\":\"1\"}}"
              << ",\"warmups\":" << kWarmups
              << ",\"iterations_per_mode_per_batch\":" << kIterations
              << ",\"allocation_scope\":\"C++ operator new/new[] calls during RRT::plan\""
              << ",\"batches\":[";
    for (std::size_t index = 0; index < batches.size(); ++index) {
      const auto& batch = batches[index];
      std::cout << (index == 0U ? "" : ",") << "{\"order\":\""
                << batch.order << "\",\"ordinary_private_context\":";
      write_summary(batch.ordinary);
      std::cout << ",\"serialized_scene_reuse\":";
      write_summary(batch.reused);
      std::cout << ",\"delta_percent\":{\"p50_ms\":"
                << percent_delta(batch.reused.p50_ms, batch.ordinary.p50_ms)
                << ",\"throughput_hz\":"
                << percent_delta(batch.reused.throughput_hz,
                                 batch.ordinary.throughput_hz)
                << ",\"operator_new_allocations_per_plan\":"
                << percent_delta(batch.reused.allocations_per_plan,
                                 batch.ordinary.allocations_per_plan)
                << ",\"operator_new_bytes_per_plan\":"
                << percent_delta(batch.reused.allocated_bytes_per_plan,
                                 batch.ordinary.allocated_bytes_per_plan)
                << "}}";
    }
    std::cout << "],\"aggregate_batch_median\":{"
                 "\"ordinary_private_context\":";
    write_summary(ordinary_median);
    std::cout << ",\"serialized_scene_reuse\":";
    write_summary(reused_median);
    std::cout << ",\"delta_percent\":{\"p50_ms\":"
              << percent_delta(reused_median.p50_ms, ordinary_median.p50_ms)
              << ",\"throughput_hz\":"
              << percent_delta(reused_median.throughput_hz,
                               ordinary_median.throughput_hz)
              << ",\"operator_new_allocations_per_plan\":"
              << percent_delta(reused_median.allocations_per_plan,
                               ordinary_median.allocations_per_plan)
              << ",\"operator_new_bytes_per_plan\":"
              << percent_delta(reused_median.allocated_bytes_per_plan,
                               ordinary_median.allocated_bytes_per_plan)
              << "}}}\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
