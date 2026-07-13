#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_roboplan/roboplan_execution_state_sink.hpp>
#include <manyforge_roboplan/roboplan_gripper_command_backend.hpp>
#include <manyforge_roboplan/roboplan_planner_options.hpp>

#include <manyforge_roboplan/roboplan_focused_adapters.hpp>

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
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (buffer_.size() >= kMaxBufferedEvents) {
            buffer_.pop_front();
            ++dropped_total_;
          }
          buffer_.push_back(event);
        }
        cv_.notify_one();
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
  cv_.notify_all();
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

bool KernelEventCollector::wait_for_events(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, timeout,
               [this] { return !buffer_.empty() || !subscribed_; });
  return !buffer_.empty();
}

// --- NativeKernelBundle ---

std::uint64_t NativeKernelBundle::start_planning_qualification_capture(
    std::size_t capacity) {
  std::lock_guard<std::mutex> close_lock(close_mutex_);
  if (closed_ || !planner) {
    throw std::runtime_error("NativeKernelBundle is closed");
  }
  std::lock_guard<std::mutex> capture_lock(planning_qualification_mutex_);
  if (planning_qualification_recorder_) {
    throw std::runtime_error(
        "planning qualification capture is already active");
  }
  auto recorder =
      std::make_shared<RoboPlanPlanningQualificationRecorder>(capacity);
  ++planning_qualification_generation_;
  if (planning_qualification_generation_ == 0) {
    ++planning_qualification_generation_;
  }
  planning_qualification_recorder_ = recorder;
  planner->set_performance_observer(std::move(recorder));
  return planning_qualification_generation_;
}

RoboPlanPlanningQualificationBatch
NativeKernelBundle::drain_planning_qualification_capture(
    std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(planning_qualification_mutex_);
  if (!planning_qualification_recorder_ ||
      generation != planning_qualification_generation_) {
    throw std::runtime_error(
        "planning qualification capture generation is not active");
  }
  return planning_qualification_recorder_->drain_with_status();
}

RoboPlanPlanningQualificationBatch
NativeKernelBundle::stop_planning_qualification_capture(
    std::uint64_t generation, std::chrono::milliseconds idle_timeout) {
  std::shared_ptr<RoboPlanPlanningQualificationRecorder> recorder;
  {
    std::lock_guard<std::mutex> lock(planning_qualification_mutex_);
    if (!planning_qualification_recorder_ ||
        generation != planning_qualification_generation_) {
      throw std::runtime_error(
          "planning qualification capture generation is not active");
    }
    planner->set_performance_observer(nullptr);
    recorder = planning_qualification_recorder_;
    recorder->close_admission();
  }
  if (!recorder->wait_until_idle(idle_timeout)) {
    throw std::runtime_error(
        "planning qualification capture still has an in-flight planner call");
  }
  {
    std::lock_guard<std::mutex> lock(planning_qualification_mutex_);
    if (planning_qualification_recorder_ != recorder ||
        generation != planning_qualification_generation_) {
      throw std::runtime_error(
          "planning qualification capture changed while stopping");
    }
    planning_qualification_recorder_.reset();
  }
  return recorder->drain_with_status();
}

std::uint64_t NativeKernelBundle::planning_qualification_generation() const {
  std::lock_guard<std::mutex> lock(planning_qualification_mutex_);
  return planning_qualification_recorder_
             ? planning_qualification_generation_
             : 0;
}

std::uint64_t NativeKernelBundle::planning_qualification_dropped_total()
    const {
  std::lock_guard<std::mutex> lock(planning_qualification_mutex_);
  return planning_qualification_recorder_
             ? planning_qualification_recorder_->dropped_total()
             : 0;
}

std::size_t NativeKernelBundle::planning_qualification_in_flight() const {
  std::lock_guard<std::mutex> lock(planning_qualification_mutex_);
  return planning_qualification_recorder_
             ? planning_qualification_recorder_->in_flight()
             : 0;
}

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
  {
    std::lock_guard<std::mutex> lock(planning_qualification_mutex_);
    if (planner) {
      planner->set_performance_observer(nullptr);
    }
    if (planning_qualification_recorder_) {
      planning_qualification_recorder_->close_admission();
    }
    planning_qualification_recorder_.reset();
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
    const core::ExecutionKernelConfig& kernel_config,
    const std::string& logical_robot_id) {

  auto bundle = std::make_shared<NativeKernelBundle>();
  const std::string robot_id =
      logical_robot_id.empty() ? model_name : logical_robot_id;
  bundle->robot_id = robot_id;
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
  const auto rrt_opts = production_rrt_options(
      group_name, kernel_config.planner_max_planning_time_s);
  bundle->rrt = std::make_shared<roboplan::RRT>(bundle->scene, rrt_opts);

  // Create PathShortcutter
  bundle->shortcutter = std::make_shared<roboplan::PathShortcutter>(
      bundle->scene, group_name);

  // Create TOPPRA
  bundle->toppra = std::make_shared<roboplan::PathParameterizerTOPPRA>(
      bundle->scene, group_name);

  // Create IK (match roboplan example_ik.cpp settings)
  const auto ik_opts = production_planning_ik_options(group_name);
  bundle->ik = std::make_shared<roboplan::SimpleIk>(bundle->scene, ik_opts);
  const auto single_shot_ik_opts = production_single_shot_ik_options(
      group_name, kernel_config.single_shot_ik_timeout_s);
  bundle->single_shot_ik =
      std::make_shared<roboplan::SimpleIk>(bundle->scene, single_shot_ik_opts);

  // Create native backends
  bundle->scene_mutex = std::make_shared<std::mutex>();
  const auto policy = kernel_config.baseline_collision_policy_snapshot.value_or(
      default_collision_policy_snapshot());
  bundle->projection_backend = std::make_shared<RoboPlanProjectionBackend>(
      bundle->scene, group_name, bundle->scene_mutex, policy, robot_id);
  bundle->scene_projection =
      std::make_shared<RoboPlanSceneProjection>(bundle->projection_backend);
  bundle->scene_authority = std::make_unique<core::SceneAuthority>(
      *bundle->scene_projection, *bundle->scene_projection);
  bundle->kinematics =
      std::make_unique<RoboPlanKinematicsAdapter>(
          bundle->scene, group_name, bundle->scene_mutex,
          *bundle->scene_authority, robot_id);
  bundle->reference_resolver =
      std::make_unique<RoboPlanReferencePoseResolver>(
          bundle->scene_projection);
  bundle->trajectory_validator =
      std::make_unique<RoboPlanTrajectoryValidator>(
          bundle->scene, group_name, bundle->scene_mutex);
  bundle->collision_trajectory_projector =
      std::make_unique<RoboPlanCollisionTrajectoryProjector>(
          bundle->scene, group_name, bundle->scene_mutex);
  bundle->collision_diagnostic_comparator =
      std::make_unique<RoboPlanCollisionDiagnosticComparator>(bundle->projection_backend);
  bundle->planner = std::make_unique<RoboPlanPlannerBackend>(
      bundle->scene_projection, bundle->scene, rrt_opts,
      bundle->scene_mutex, bundle->single_shot_ik, ik_opts,
      single_shot_ik_opts, kernel_config.planner_rng_seed);
  bundle->clock = std::make_unique<WallClock>();
  RoboPlanExecutionBackendConfig execution_config;
  execution_config.reset_completion_timeout_s =
      kernel_config.native_reset_completion_timeout_s;
  auto observed_joint_names = bundle->kinematics->group_joint_names(group_name);
  if (!observed_joint_names.ok()) {
    throw std::runtime_error(
        "native execution joint order is unavailable: " +
        observed_joint_names.error().message);
  }
  execution_config.observed_joint_names =
      std::move(observed_joint_names.value());
  auto native_execution = std::make_unique<RoboPlanExecutionBackend>(
      bundle->scene, group_name, *bundle->clock, bundle->scene_mutex,
      execution_config);
  auto* native_execution_ptr = native_execution.get();
  bundle->execution = std::move(native_execution);
  auto initial_observed_state =
      bundle->execution->sync_observed_state_before_planning();
  if (!initial_observed_state.ok()) {
    throw std::runtime_error(
        "native execution backend could not publish its initial observed "
        "state: " +
        initial_observed_state.error().message);
  }
  const auto* observed_publication =
      bundle->execution->observed_robot_state_publication();
  if (observed_publication == nullptr) {
    throw std::runtime_error(
        "native execution backend did not publish observed robot state");
  }
  bundle->observed_state_provider =
      std::make_unique<core::PublishedObservedRobotStateProvider>(
          *observed_publication,
          core::ObservedRobotStateAuthorityProfile::kPredictedNative,
          native_execution_ptr->observed_state_authority_epoch());
  bundle->scheduler = std::make_unique<SimpleScheduler>();
  bundle->cache = std::make_unique<core::TrajectoryCache>();
  bundle->execution_state_sink =
      std::make_shared<RoboPlanExecutionStateSink>(
          bundle->scene, bundle->scene_mutex);
  bundle->owned_gripper_command_backend =
      std::make_unique<RoboPlanGripperCommandBackend>(
          bundle->execution_state_sink, *bundle->clock, *bundle->scheduler);
  bundle->gripper_command_backend =
      bundle->owned_gripper_command_backend.get();

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
    collision_resources.snapshot_publisher = bundle->scene_authority.get();
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
  // This factory publishes an explicitly qualified predicted-native source,
  // never measured robot feedback. Derive recovery policy from that authority
  // instead of leaving the backend-neutral measured default enabled.
  resolved_kernel_config.recovery_require_measured_observation = false;
  if (resolved_kernel_config.recovery_observed_joint_order_token.empty()) {
    const auto* observed =
        bundle->execution->observed_robot_state_publication();
    if (observed != nullptr) {
      resolved_kernel_config.recovery_observed_joint_order_token =
          observed->joint_order_token();
    }
  }

  // Wire kernel — references to owned components
  bundle->kernel = std::make_shared<core::ExecutionKernel>(
      *bundle->planner,
      core::ExecutionKernelPorts{
          *bundle->observed_state_provider,
          *bundle->kinematics,
          *bundle->reference_resolver,
          *bundle->trajectory_validator,
          *bundle->collision_trajectory_projector,
          bundle->collision_diagnostic_comparator.get(),
          bundle->gripper_command_backend},
      *bundle->execution,
      *bundle->clock, *bundle->scheduler, *bundle->cache,
      resolved_kernel_config);
  bundle->kernel->set_collision_validation_policy_snapshot(policy);
  bundle->kernel->set_collision_resources(
      *bundle->scene_authority,
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
    const core::ExecutionKernelConfig& kernel_config,
    const std::string& logical_robot_id) {

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
                              {pkg_share.string()}, kernel_config,
                              logical_robot_id);
}

}  // namespace manyforge::roboplan_backend
