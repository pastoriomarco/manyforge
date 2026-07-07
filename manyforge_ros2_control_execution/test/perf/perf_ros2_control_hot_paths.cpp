#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#define MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#undef MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
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

using manyforge::core::ExecutionStateSinkResult;
using manyforge::core::ExecutionStateSinkStatus;
using manyforge::core::ExecutionStateUpdateMode;
using manyforge::ros2_control_execution::Ros2ControlExecutionBackend;
using manyforge::ros2_control_execution::Ros2ControlExecutionBackendConfig;

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

std::string format_double(double value) {
  std::ostringstream out;
  out << std::setprecision(12) << value;
  return out.str();
}

struct AllocationProbeResult {
  std::size_t iterations = 0;
  std::size_t allocations = 0;
  double allocations_per_iteration = 0.0;
  double total_us = 0.0;
  double mean_us = 0.0;
  std::size_t checksum = 0;
};

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

class CountingExecutionStateSink final
    : public manyforge::core::IExecutionStateSink {
 public:
  ExecutionStateSinkResult apply_group_joint_positions(
      const std::string&,
      const std::vector<double>& positions_rad,
      ExecutionStateUpdateMode) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++group_updates_;
    for (const double value : positions_rad) {
      checksum_ += value;
    }
    return {ExecutionStateSinkStatus::kApplied};
  }

  std::optional<double> group_joint_position(
      const std::string&,
      const std::string&) const override {
    return std::nullopt;
  }

  ExecutionStateSinkResult apply_group_joint_position(
      const std::string&,
      const std::string&,
      double,
      ExecutionStateUpdateMode) override {
    return {ExecutionStateSinkStatus::kApplied};
  }

  std::size_t group_updates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return group_updates_;
  }

  double checksum() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return checksum_;
  }

 private:
  mutable std::mutex mutex_;
  std::size_t group_updates_ = 0;
  double checksum_ = 0.0;
};

Ros2ControlExecutionBackendConfig make_config(std::vector<std::string> joints) {
  Ros2ControlExecutionBackendConfig config;
  config.node_name = "manyforge_ros2_control_hot_path_qualification";
  config.action_name = "/manyforge_ros2_control_hot_path_qualification_fjt";
  config.joint_state_topic =
      "/manyforge_ros2_control_hot_path_qualification_joint_states";
  config.controller_state_topic.clear();
  config.required_joints = std::move(joints);
  config.wait_for_state_timeout_s = 0.0;
  config.wait_for_action_server_timeout_s = 0.0;
  config.monitor_period_s = 0.01;
  config.joint_state_max_age_s = 3600.0;
  config.enable_jtc_speed_scaling_input = false;
  return config;
}

std::vector<std::string> make_joint_names(std::size_t count) {
  std::vector<std::string> names;
  names.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    names.push_back("j" + std::to_string(i));
  }
  return names;
}

std::vector<double> make_positions(std::size_t count, double offset = 0.0) {
  std::vector<double> positions;
  positions.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    positions.push_back(offset + 0.01 * static_cast<double>(i));
  }
  return positions;
}

AllocationProbeResult measure_feedback_storage_allocations(
    Ros2ControlExecutionBackend& backend,
    const std::vector<std::string>& warm_names,
    const std::vector<double>& warm_positions,
    const std::vector<std::string>& hot_names,
    const std::vector<double>& hot_positions,
    std::size_t iterations) {
  const std::vector<double> no_velocities;
  const auto t0 = std::chrono::steady_clock::now();
  backend.testing_store_action_feedback_sample(
      warm_names, warm_positions, no_velocities, t0);
  backend.testing_store_action_feedback_sample(
      hot_names, hot_positions, no_velocities, t0 + std::chrono::nanoseconds(1));
  return measure_allocations(iterations, [&]() {
    std::size_t checksum = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
      backend.testing_store_action_feedback_sample(
          hot_names,
          hot_positions,
          no_velocities,
          t0 + std::chrono::nanoseconds(static_cast<long>(i + 2)));
      checksum += backend.testing_latest_action_feedback_position_capacity();
    }
    return checksum;
  });
}

AllocationProbeResult measure_idle_repeated_sample_skip_allocations(
    Ros2ControlExecutionBackend& backend,
    const std::vector<std::string>& names,
    const std::vector<double>& positions,
    std::size_t iterations) {
  const auto t0 = std::chrono::steady_clock::now();
  backend.testing_store_latest_joint_sample(names, positions, t0);
  backend.testing_sync_idle_scene_from_latest_sample();
  return measure_allocations(iterations, [&]() {
    std::size_t checksum = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
      backend.testing_sync_idle_scene_from_latest_sample();
      checksum += positions.size();
    }
    return checksum;
  });
}

}  // namespace

int main() {
  const std::size_t iterations =
      read_size_env("MANYFORGE_ROS2_CONTROL_HOT_PATH_ITERATIONS", 10000);
  const auto feedback_allocation_budget =
      read_optional_size_env("MANYFORGE_ROS2_CONTROL_FEEDBACK_ALLOC_BUDGET");
  const auto idle_skip_allocation_budget =
      read_optional_size_env("MANYFORGE_ROS2_CONTROL_IDLE_SKIP_ALLOC_BUDGET");
  const auto idle_skip_update_budget =
      read_optional_size_env("MANYFORGE_ROS2_CONTROL_IDLE_SKIP_MAX_UPDATES");

  auto hot_names = make_joint_names(6);
  auto hot_positions = make_positions(hot_names.size());
  auto warm_names = make_joint_names(64);
  auto warm_positions = make_positions(warm_names.size(), 1.0);

  manyforge::core::FakeClock feedback_clock;
  auto feedback_sink = std::make_shared<CountingExecutionStateSink>();
  Ros2ControlExecutionBackend feedback_backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      feedback_sink,
      "arm",
      feedback_clock,
      make_config(hot_names));
  const auto feedback_allocations = measure_feedback_storage_allocations(
      feedback_backend,
      warm_names,
      warm_positions,
      hot_names,
      hot_positions,
      iterations);

  manyforge::core::FakeClock idle_clock;
  auto idle_sink = std::make_shared<CountingExecutionStateSink>();
  Ros2ControlExecutionBackend idle_backend(
      Ros2ControlExecutionBackend::TestingNoRosStartupTag{},
      idle_sink,
      "arm",
      idle_clock,
      make_config(hot_names));
  const auto idle_skip_allocations =
      measure_idle_repeated_sample_skip_allocations(
          idle_backend, hot_names, hot_positions, iterations);
  const std::size_t idle_group_updates = idle_sink->group_updates();

  bool budget_pass = true;
  if (feedback_allocation_budget.has_value() &&
      feedback_allocations.allocations > *feedback_allocation_budget) {
    budget_pass = false;
  }
  if (idle_skip_allocation_budget.has_value() &&
      idle_skip_allocations.allocations > *idle_skip_allocation_budget) {
    budget_pass = false;
  }
  if (idle_skip_update_budget.has_value() &&
      idle_group_updates > *idle_skip_update_budget) {
    budget_pass = false;
  }

  std::ostringstream out;
  out << "{"
      << "\"scenario\":\"ws8_ros2_control_hot_path_qualification\","
      << "\"iterations\":" << iterations << ","
      << "\"joint_count\":" << hot_names.size() << ","
      << "\"warm_joint_count\":" << warm_names.size() << ","
      << "\"allocation_counts\":{\"action_feedback_reused_storage\":{"
      << allocation_probe_json(feedback_allocations)
      << "},\"idle_repeated_sample_skip\":{"
      << allocation_probe_json(idle_skip_allocations)
      << "}},\"idle_scene_updates_after_repeated_sync\":"
      << idle_group_updates
      << ",\"idle_scene_update_checksum\":"
      << format_double(idle_sink->checksum())
      << ",\"budgets\":{\"action_feedback_reused_storage_allocations\":";
  if (feedback_allocation_budget.has_value()) {
    out << *feedback_allocation_budget;
  } else {
    out << "null";
  }
  out << ",\"idle_repeated_sample_skip_allocations\":";
  if (idle_skip_allocation_budget.has_value()) {
    out << *idle_skip_allocation_budget;
  } else {
    out << "null";
  }
  out << ",\"idle_repeated_sample_max_updates\":";
  if (idle_skip_update_budget.has_value()) {
    out << *idle_skip_update_budget;
  } else {
    out << "null";
  }
  out << ",\"passed\":" << (budget_pass ? "true" : "false") << "}}";

  const std::string json = out.str();
  if (const char* out_path =
          std::getenv("MANYFORGE_ROS2_CONTROL_HOT_PATH_JSON_OUT");
      out_path != nullptr && *out_path != '\0') {
    std::ofstream f(out_path);
    if (!f) {
      std::cerr << "failed to open MANYFORGE_ROS2_CONTROL_HOT_PATH_JSON_OUT="
                << out_path << "\n";
      return 2;
    }
    f << json << "\n";
  }

  std::cout << "MANYFORGE_QUALIFICATION_EVIDENCE " << json << "\n";
  return budget_pass ? 0 : 3;
}
