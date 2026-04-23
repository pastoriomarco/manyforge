#include <manyforge_roboplan/native_kernel_factory.hpp>

#include <roboplan_example_models/resources.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace manyforge::roboplan_backend {

// --- KernelEventCollector ---

void KernelEventCollector::subscribe(core::ExecutionKernel& kernel) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (subscribed_) return;
  subscription_ = kernel.subscribe_events(
      [this](const core::KernelEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (buffer_.size() >= kMaxBufferedEvents) {
          buffer_.pop_front();
        }
        buffer_.push_back(event);
      });
  subscribed_ = true;
}

void KernelEventCollector::unsubscribe(core::ExecutionKernel& kernel) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!subscribed_) return;
  kernel.unsubscribe_events(subscription_);
  subscribed_ = false;
}

std::vector<core::KernelEvent> KernelEventCollector::drain() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<core::KernelEvent> result(buffer_.begin(), buffer_.end());
  buffer_.clear();
  return result;
}

// --- NativeKernelBundle ---

NativeKernelBundle::~NativeKernelBundle() {
  // Unsubscribe before kernel destruction to avoid dangling callback
  if (kernel) {
    event_collector.unsubscribe(*kernel);
  }
}

// --- WallClock ---

core::TimePoint WallClock::now() const {
  auto tp = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             tp.time_since_epoch())
      .count();
}

// --- SimpleScheduler ---

namespace {

core::TimePoint steady_now_ns() {
  auto tp = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             tp.time_since_epoch())
      .count();
}

}  // namespace

SimpleScheduler::SimpleScheduler() = default;

SimpleScheduler::~SimpleScheduler() {
  if (worker_thread_.joinable()) {
    worker_thread_.request_stop();
    cv_.notify_all();
  }
}

void SimpleScheduler::ensure_worker_started_locked() {
  if (worker_thread_.joinable()) {
    return;
  }
  worker_thread_ = std::jthread([this](std::stop_token st) { worker_loop(st); });
}

core::ScheduleHandle SimpleScheduler::schedule(
    core::TimePoint deadline, std::function<void()> callback) {
  auto handle = next_handle_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_worker_started_locked();
    tasks_.emplace(handle, ScheduledTask{deadline, std::move(callback)});
  }
  cv_.notify_all();
  return handle;
}

void SimpleScheduler::cancel(core::ScheduleHandle handle) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.erase(handle);
  }
  cv_.notify_all();
}

void SimpleScheduler::worker_loop(std::stop_token st) {
  while (!st.stop_requested()) {
    std::function<void()> callback;

    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this, &st]() { return st.stop_requested() || !tasks_.empty(); });
      if (st.stop_requested()) {
        return;
      }

      const auto next_it = std::min_element(
          tasks_.begin(), tasks_.end(),
          [](const auto& lhs, const auto& rhs) {
            return lhs.second.deadline < rhs.second.deadline;
          });

      const auto now = steady_now_ns();
      if (next_it->second.deadline > now) {
        cv_.wait_for(lock, std::chrono::nanoseconds(next_it->second.deadline - now));
        continue;
      }

      callback = std::move(next_it->second.callback);
      tasks_.erase(next_it);
    }

    if (callback) {
      callback();
    }
  }
}

// --- Model path resolution ---

namespace {

struct ModelConfig {
  std::string urdf_rel;
  std::string srdf_rel;
  std::string default_group;
};

const std::map<std::string, ModelConfig>& model_configs() {
  static const std::map<std::string, ModelConfig> configs = {
      {"ur5_gripper",
       {"ur_robot_model/ur5_gripper.urdf",
        "ur_robot_model/ur5_gripper.srdf", "arm"}},
      {"fr3",
       {"franka_robot_model/fr3.urdf",
        "franka_robot_model/fr3.srdf", "fr3_arm"}},
      {"kinova",
       {"kinova_robot_model/kinova_robotiq.urdf",
        "kinova_robot_model/kinova_robotiq.srdf", "kinova_arm"}},
      {"so101",
       {"so101_robot_model/so101.urdf",
        "so101_robot_model/so101.srdf", "arm"}},
  };
  return configs;
}

}  // namespace

// --- Factory (explicit paths) ---

std::shared_ptr<NativeKernelBundle> create_native_kernel(
    const std::string& model_name,
    const std::string& group_name,
    const std::string& urdf_path,
    const std::string& srdf_path,
    const std::vector<std::string>& package_paths,
    const core::ExecutionKernelConfig& kernel_config) {

  auto bundle = std::make_shared<NativeKernelBundle>();
  bundle->group_name = group_name;

  // Convert string paths to filesystem paths
  std::vector<std::filesystem::path> pkg_paths;
  for (const auto& p : package_paths) {
    pkg_paths.emplace_back(p);
  }

  // Load scene
  bundle->scene = std::make_shared<roboplan::Scene>(
      model_name, std::filesystem::path(urdf_path),
      std::filesystem::path(srdf_path), pkg_paths);

  // Create RRT
  roboplan::RRTOptions rrt_opts;
  rrt_opts.group_name = group_name;
  rrt_opts.max_planning_time = 5.0;
  rrt_opts.max_nodes = 5000;
  bundle->rrt = std::make_shared<roboplan::RRT>(bundle->scene, rrt_opts);

  // Create PathShortcutter
  bundle->shortcutter = std::make_shared<roboplan::PathShortcutter>(
      bundle->scene, group_name);

  // Create TOPPRA
  bundle->toppra = std::make_shared<roboplan::PathParameterizerTOPPRA>(
      bundle->scene, group_name);

  // Create IK (match roboplan example_ik.cpp settings)
  roboplan::SimpleIkOptions ik_opts;
  ik_opts.group_name = group_name;
  ik_opts.step_size = 0.25;
  ik_opts.max_restarts = 50;
  ik_opts.max_time = 1.0;
  bundle->ik = std::make_shared<roboplan::SimpleIk>(bundle->scene, ik_opts);

  // Create native backends
  auto scene_mutex = std::make_shared<std::mutex>();
  bundle->planner = std::make_unique<RoboPlanPlannerBackend>(
      bundle->scene, rrt_opts, bundle->shortcutter, bundle->toppra, bundle->ik);
  bundle->context = std::make_unique<RoboPlanPlanningContext>(
      bundle->scene, group_name, scene_mutex);
  bundle->clock = std::make_unique<WallClock>();
  bundle->execution = std::make_unique<RoboPlanExecutionBackend>(
      bundle->scene, group_name, *bundle->clock, scene_mutex);
  bundle->scheduler = std::make_unique<SimpleScheduler>();
  bundle->cache = std::make_unique<core::TrajectoryCache>();

  // Wire kernel — references to owned components
  bundle->kernel = std::make_unique<core::ExecutionKernel>(
      *bundle->planner, *bundle->context, *bundle->execution,
      *bundle->clock, *bundle->scheduler, *bundle->cache, kernel_config);

  // Auto-subscribe event collector
  bundle->event_collector.subscribe(*bundle->kernel);

  return bundle;
}

// --- Factory (convenience, uses example_models) ---

std::shared_ptr<NativeKernelBundle> create_native_kernel(
    const std::string& model_name,
    const std::string& group_name,
    const core::ExecutionKernelConfig& kernel_config) {

  auto it = model_configs().find(model_name);
  if (it == model_configs().end()) {
    throw std::runtime_error("Unknown model: " + model_name);
  }

  const auto& config = it->second;
  std::string actual_group = group_name.empty() ? config.default_group : group_name;

  auto models_dir = roboplan::example_models::get_package_models_dir();
  auto pkg_share = roboplan::example_models::get_package_share_dir();

  std::string urdf = (models_dir / config.urdf_rel).string();
  std::string srdf = (models_dir / config.srdf_rel).string();

  return create_native_kernel(model_name, actual_group, urdf, srdf,
                              {pkg_share.string()}, kernel_config);
}

}  // namespace manyforge::roboplan_backend
