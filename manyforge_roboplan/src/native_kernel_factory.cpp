#include <manyforge_roboplan/native_kernel_factory.hpp>

#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>

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
  core::SubscriptionHandle subscription{0};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!subscribed_) return;
    subscription = subscription_;
    subscribed_ = false;
  }
  kernel.unsubscribe_events(subscription);
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

SimpleScheduler::SimpleScheduler() {
  safety_worker_.owner = this;
  monitoring_worker_.owner = this;
}

SimpleScheduler::~SimpleScheduler() {
  for (Worker* w : {&safety_worker_, &monitoring_worker_}) {
    if (w->thread.joinable()) {
      w->thread.request_stop();
      w->cv.notify_all();
    }
  }
}

void SimpleScheduler::Worker::ensure_started_locked(Worker* self) {
  if (thread.joinable()) {
    return;
  }
  thread = std::jthread([self](std::stop_token st) { self->worker_loop(st); });
}

SimpleScheduler::Worker& SimpleScheduler::worker_for(
    core::SchedulerPriority priority) {
  switch (priority) {
    case core::SchedulerPriority::Monitoring:
      return monitoring_worker_;
    case core::SchedulerPriority::Safety:
    default:
      return safety_worker_;
  }
}

core::ScheduleHandle SimpleScheduler::schedule(
    core::TimePoint deadline, std::function<void()> callback,
    core::SchedulerPriority priority) {
  auto handle = next_handle_.fetch_add(1);
  Worker& w = worker_for(priority);
  {
    std::lock_guard<std::mutex> registry_lock(registry_mutex_);
    handle_to_priority_[handle] = priority;
  }
  {
    std::lock_guard<std::mutex> lock(w.mutex);
    w.ensure_started_locked(&w);
    w.tasks.emplace(handle, ScheduledTask{deadline, std::move(callback)});
  }
  w.cv.notify_all();
  return handle;
}

void SimpleScheduler::cancel(core::ScheduleHandle handle) {
  core::SchedulerPriority priority;
  {
    std::lock_guard<std::mutex> registry_lock(registry_mutex_);
    auto it = handle_to_priority_.find(handle);
    if (it == handle_to_priority_.end()) {
      return;  // already fired or never scheduled
    }
    priority = it->second;
    handle_to_priority_.erase(it);
  }
  Worker& w = worker_for(priority);
  {
    std::lock_guard<std::mutex> lock(w.mutex);
    w.tasks.erase(handle);
  }
  w.cv.notify_all();
}

void SimpleScheduler::Worker::worker_loop(std::stop_token st) {
  while (!st.stop_requested()) {
    core::ScheduleHandle fired_handle = 0;
    std::function<void()> callback;

    {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock, [this, &st]() { return st.stop_requested() || !tasks.empty(); });
      if (st.stop_requested()) {
        return;
      }

      const auto next_it = std::min_element(
          tasks.begin(), tasks.end(),
          [](const auto& lhs, const auto& rhs) {
            return lhs.second.deadline < rhs.second.deadline;
          });

      const auto now = steady_now_ns();
      if (next_it->second.deadline > now) {
        cv.wait_for(lock, std::chrono::nanoseconds(next_it->second.deadline - now));
        continue;
      }

      callback = std::move(next_it->second.callback);
      fired_handle = next_it->first;
      tasks.erase(next_it);
    }

    if (owner != nullptr && fired_handle != 0) {
      std::lock_guard<std::mutex> registry_lock(owner->registry_mutex_);
      owner->handle_to_priority_.erase(fired_handle);
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

core::CollisionPolicySnapshot default_collision_policy_snapshot() {
  core::CollisionPolicySnapshot policy;
  policy.policy_id = "default_free_space";
  policy.revision.revision_id = 1;
  policy.revision.content_hash = "default_free_space_v1";
  policy.require_full_diagnostics = false;
  return policy;
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
  auto single_shot_ik_opts = ik_opts;
  single_shot_ik_opts.max_time = kernel_config.single_shot_ik_timeout_s;
  bundle->single_shot_ik =
      std::make_shared<roboplan::SimpleIk>(bundle->scene, single_shot_ik_opts);

  // Create native backends
  auto scene_mutex = std::make_shared<std::mutex>();
  bundle->planner = std::make_unique<RoboPlanPlannerBackend>(
      bundle->scene, rrt_opts, bundle->shortcutter, bundle->toppra, bundle->ik,
      scene_mutex, bundle->single_shot_ik);
  bundle->context = std::make_unique<RoboPlanPlanningContext>(
      bundle->scene, group_name, scene_mutex);
  bundle->clock = std::make_unique<WallClock>();
  RoboPlanExecutionBackendConfig execution_config;
  execution_config.reset_completion_timeout_s =
      kernel_config.native_reset_completion_timeout_s;
  bundle->execution = std::make_unique<RoboPlanExecutionBackend>(
      bundle->scene, group_name, *bundle->clock, scene_mutex, execution_config);
  bundle->scheduler = std::make_unique<SimpleScheduler>();
  bundle->cache = std::make_unique<core::TrajectoryCache>();

  const auto policy = default_collision_policy_snapshot();
  bundle->context->set_collision_policy_snapshot(policy);

  const int collision_worker_count =
      core::env_validation_collision_workers(
          std::max(1, kernel_config.validation_collision_workers));
  bundle->collision_validation_engines.reserve(collision_worker_count);
  for (int i = 0; i < collision_worker_count; ++i) {
    bundle->collision_validation_engines.push_back(
        std::make_shared<manyforge::collision::ManyForgeCollisionEngine>(
            model_name,
            std::filesystem::path(urdf_path),
            std::filesystem::path(srdf_path),
            pkg_paths,
            group_name));
  }

  core::ExecutionKernelConfig resolved_kernel_config = kernel_config;
  resolved_kernel_config.validation_collision_workers = collision_worker_count;

  // Wire kernel — references to owned components
  bundle->kernel = std::make_unique<core::ExecutionKernel>(
      *bundle->planner, *bundle->context, *bundle->execution,
      *bundle->clock, *bundle->scheduler, *bundle->cache,
      resolved_kernel_config);
  bundle->kernel->set_collision_validation_policy_snapshot(policy);
  bundle->kernel->set_collision_validation_pipeline(
      *bundle->context,
      bundle->collision_validation_engines);

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
