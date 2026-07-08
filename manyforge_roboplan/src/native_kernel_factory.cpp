#include <manyforge_roboplan/native_kernel_factory.hpp>

#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/collision/collision_resource_manager.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>

#include <roboplan_example_models/resources.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#endif

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
          ++dropped_total_;
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

KernelEventCollector::DrainStats KernelEventCollector::drain_with_stats() {
  std::lock_guard<std::mutex> lock(mutex_);
  DrainStats stats;
  stats.events.assign(buffer_.begin(), buffer_.end());
  buffer_.clear();
  stats.dropped_total = dropped_total_;
  return stats;
}

// --- NativeKernelBundle ---

void NativeKernelBundle::close() {
  std::shared_ptr<core::ExecutionKernel> kernel_to_close;
  {
    std::lock_guard<std::mutex> lock(close_mutex_);
    if (closed_) {
      return;
    }
    closed_ = true;
    if (kernel) {
      event_collector.unsubscribe(*kernel);
    }
    kernel_to_close = std::move(kernel);
  }
  kernel_to_close.reset();
}

bool NativeKernelBundle::closed() const {
  std::lock_guard<std::mutex> lock(close_mutex_);
  return closed_;
}

std::shared_ptr<core::ExecutionKernel> NativeKernelBundle::kernel_snapshot() const {
  std::lock_guard<std::mutex> lock(close_mutex_);
  if (closed_ || !kernel) {
    throw std::runtime_error("NativeKernelBundle is closed");
  }
  return kernel;
}

NativeKernelBundle::~NativeKernelBundle() {
  close();
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

bool scheduler_priority_terminates_on_exception(
    core::SchedulerPriority priority) {
  switch (priority) {
    case core::SchedulerPriority::ControllerCommand:
    case core::SchedulerPriority::RuntimeSafety:
    case core::SchedulerPriority::StopControl:
      return true;
    case core::SchedulerPriority::Lifecycle:
    case core::SchedulerPriority::Monitoring:
    case core::SchedulerPriority::Background:
      return false;
  }
  return true;
}

void log_scheduler_callback_exception(core::SchedulerPriority priority,
                                      const char* exception_text,
                                      bool terminating) {
  std::cerr << "manyforge_scheduler_callback_exception"
            << " priority=" << core::scheduler_priority_name(priority)
            << " terminating=" << (terminating ? "true" : "false")
            << " exception=" << exception_text << std::endl;
}

}  // namespace

SimpleScheduler::SimpleScheduler() {
  for (std::size_t i = 0; i < workers_.size(); ++i) {
    workers_[i].owner = this;
    workers_[i].priority = core::scheduler_priority_at(i);
    workers_[i].thread_name = [&]() -> const char* {
      switch (core::scheduler_priority_at(i)) {
        case core::SchedulerPriority::ControllerCommand:
          return "mf_sched_ctrl";
        case core::SchedulerPriority::RuntimeSafety:
          return "mf_sched_safe";
        case core::SchedulerPriority::StopControl:
          return "mf_sched_stop";
        case core::SchedulerPriority::Lifecycle:
          return "mf_sched_life";
        case core::SchedulerPriority::Monitoring:
          return "mf_sched_mon";
        case core::SchedulerPriority::Background:
          return "mf_sched_bg";
      }
      return "mf_sched";
    }();
  }
}

SimpleScheduler::~SimpleScheduler() {
  for (auto& worker : workers_) {
    if (worker.thread.joinable()) {
      worker.thread.request_stop();
      worker.cv.notify_all();
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
  return workers_[core::scheduler_priority_index(priority)];
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
    ++w.queue_generation;
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
    ++w.queue_generation;
    w.tasks.erase(handle);
  }
  w.cv.notify_all();
}

void SimpleScheduler::Worker::drain() {
  std::unique_lock<std::mutex> lock(mutex);
  if (thread.joinable() && std::this_thread::get_id() == thread.get_id()) {
    return;
  }
  drain_cv.wait(lock, [this]() { return callbacks_in_flight == 0; });
}

bool SimpleScheduler::Worker::drain_for(std::chrono::nanoseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex);
  if (thread.joinable() && std::this_thread::get_id() == thread.get_id()) {
    return true;
  }
  return drain_cv.wait_for(
      lock, timeout, [this]() { return callbacks_in_flight == 0; });
}

void SimpleScheduler::drain(core::SchedulerPriority priority) {
  worker_for(priority).drain();
}

bool SimpleScheduler::drain_for(core::SchedulerPriority priority,
                                std::chrono::nanoseconds timeout) {
  return worker_for(priority).drain_for(timeout);
}

void SimpleScheduler::drain_all() {
  for (auto& worker : workers_) {
    worker.drain();
  }
}

bool SimpleScheduler::drain_all_for(std::chrono::nanoseconds timeout) {
  bool drained = true;
  for (auto& worker : workers_) {
    drained = worker.drain_for(timeout) && drained;
  }
  return drained;
}

void SimpleScheduler::Worker::worker_loop(std::stop_token st) {
#ifdef __linux__
  if (thread_name != nullptr) {
    (void)pthread_setname_np(pthread_self(), thread_name);
  }
#endif
  while (!st.stop_requested()) {
    core::ScheduleHandle fired_handle = 0;
    std::function<void()> callback;

    {
      std::unique_lock<std::mutex> lock(mutex);
      if (!cv.wait(lock, st, [this]() { return !tasks.empty(); })) {
        return;
      }

      const auto next_it = std::min_element(
          tasks.begin(), tasks.end(),
          [](const auto& lhs, const auto& rhs) {
            return lhs.second.deadline < rhs.second.deadline;
          });

      const auto now = steady_now_ns();
      if (next_it->second.deadline > now) {
        const auto observed_generation = queue_generation;
        (void)cv.wait_for(
            lock,
            st,
            std::chrono::nanoseconds(next_it->second.deadline - now),
            [this, observed_generation]() {
              return queue_generation != observed_generation;
            });
        continue;
      }

      callback = std::move(next_it->second.callback);
      fired_handle = next_it->first;
      tasks.erase(next_it);
      ++callbacks_in_flight;
    }

    if (owner != nullptr && fired_handle != 0) {
      std::lock_guard<std::mutex> registry_lock(owner->registry_mutex_);
      owner->handle_to_priority_.erase(fired_handle);
    }

    const auto finish_callback = [this]() {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (callbacks_in_flight > 0) {
          --callbacks_in_flight;
        }
      }
      drain_cv.notify_all();
    };

    if (callback) {
      try {
        callback();
      } catch (const std::exception& e) {
        finish_callback();
        const bool terminating =
            scheduler_priority_terminates_on_exception(priority);
        log_scheduler_callback_exception(priority, e.what(), terminating);
        if (terminating) {
          std::terminate();
        }
        continue;
      } catch (...) {
        finish_callback();
        const bool terminating =
            scheduler_priority_terminates_on_exception(priority);
        log_scheduler_callback_exception(priority,
                                         "unknown_non_std_exception",
                                         terminating);
        if (terminating) {
          std::terminate();
        }
        continue;
      }
    }

    finish_callback();
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
  rrt_opts.max_planning_time = kernel_config.planner_max_planning_time_s;
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
  bundle->scene_mutex = std::make_shared<std::mutex>();
  const auto policy = kernel_config.baseline_collision_policy_snapshot.value_or(
      default_collision_policy_snapshot());
  bundle->planner = std::make_unique<RoboPlanPlannerBackend>(
      bundle->scene, rrt_opts, bundle->shortcutter, bundle->toppra, bundle->ik,
      bundle->scene_mutex, bundle->single_shot_ik, ik_opts,
      single_shot_ik_opts);
  bundle->context = std::make_unique<RoboPlanPlanningContext>(
      bundle->scene, group_name, bundle->scene_mutex, policy);
  bundle->clock = std::make_unique<WallClock>();
  RoboPlanExecutionBackendConfig execution_config;
  execution_config.reset_completion_timeout_s =
      kernel_config.native_reset_completion_timeout_s;
  bundle->execution = std::make_unique<RoboPlanExecutionBackend>(
      bundle->scene, group_name, *bundle->clock, bundle->scene_mutex,
      execution_config);
  bundle->scheduler = std::make_unique<SimpleScheduler>();
  bundle->cache = std::make_unique<core::TrajectoryCache>();

  const int collision_worker_count =
      core::env_validation_collision_workers(
          std::max(1, kernel_config.validation_collision_workers));
  bundle->collision_runtime_monitor_engine =
      std::make_shared<manyforge::collision::ManyForgeCollisionEngine>(
          model_name,
          std::filesystem::path(urdf_path),
          std::filesystem::path(srdf_path),
          pkg_paths,
          group_name);
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
  {
    core::CollisionPipelineResources collision_resources;
    collision_resources.snapshot_publisher = bundle->context.get();
    collision_resources.runtime_monitor_engine =
        bundle->collision_runtime_monitor_engine;
    collision_resources.validation_engines =
        bundle->collision_validation_engines;
    const auto admission =
        core::CollisionResourceManager::validate_admission(
            collision_resources, /*runtime_protection_active=*/true);
    if (!admission.admitted()) {
      throw std::runtime_error(
          "native kernel collision resources are not runtime-safe: " +
          admission.resource_conflict_reason);
    }
  }

  core::ExecutionKernelConfig resolved_kernel_config = kernel_config;
  resolved_kernel_config.validation_collision_workers = collision_worker_count;

  // Wire kernel — references to owned components
  bundle->kernel = std::make_shared<core::ExecutionKernel>(
      *bundle->planner, *bundle->context, *bundle->execution,
      *bundle->clock, *bundle->scheduler, *bundle->cache,
      resolved_kernel_config);
  bundle->kernel->set_collision_validation_policy_snapshot(policy);
  bundle->kernel->set_collision_resources(
      *bundle->context,
      bundle->collision_runtime_monitor_engine,
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
