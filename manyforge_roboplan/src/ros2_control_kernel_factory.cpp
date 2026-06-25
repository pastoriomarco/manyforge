#include <manyforge_roboplan/ros2_control_kernel_factory.hpp>

#include <algorithm>
#include <filesystem>
#include <mutex>

#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_roboplan/roboplan_execution_state_sink.hpp>
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>

namespace manyforge::roboplan_backend {
namespace ros2_exec = ::manyforge::ros2_control_execution;
namespace {

core::CollisionPolicySnapshot default_collision_policy_snapshot() {
  core::CollisionPolicySnapshot policy;
  policy.policy_id = "default_free_space";
  policy.revision.revision_id = 1;
  policy.revision.content_hash = "default_free_space_v1";
  policy.require_full_diagnostics = false;
  return policy;
}

}  // namespace

std::shared_ptr<NativeKernelBundle> create_ros2_control_kernel(
    const std::string& model_name,
    const std::string& group_name,
    const std::string& urdf_path,
    const std::string& srdf_path,
    const std::vector<std::string>& package_paths,
    const ros2_exec::Ros2ControlExecutionBackendConfig& ros_config,
    const core::ExecutionKernelConfig& kernel_config) {
  auto bundle = std::make_shared<NativeKernelBundle>();
  bundle->group_name = group_name;

  std::vector<std::filesystem::path> pkg_paths;
  for (const auto& path : package_paths) {
    pkg_paths.emplace_back(path);
  }

  bundle->scene = std::make_shared<roboplan::Scene>(
      model_name, std::filesystem::path(urdf_path),
      std::filesystem::path(srdf_path), pkg_paths);

  roboplan::RRTOptions rrt_opts;
  rrt_opts.group_name = group_name;
  rrt_opts.max_planning_time = kernel_config.planner_max_planning_time_s;
  rrt_opts.max_nodes = 5000;
  bundle->rrt = std::make_shared<roboplan::RRT>(bundle->scene, rrt_opts);
  bundle->shortcutter = std::make_shared<roboplan::PathShortcutter>(
      bundle->scene, group_name);
  bundle->toppra = std::make_shared<roboplan::PathParameterizerTOPPRA>(
      bundle->scene, group_name);

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

  auto scene_mutex = std::make_shared<std::mutex>();
  bundle->planner = std::make_unique<RoboPlanPlannerBackend>(
      bundle->scene, rrt_opts, bundle->shortcutter, bundle->toppra, bundle->ik,
      scene_mutex, bundle->single_shot_ik);
  bundle->context = std::make_unique<RoboPlanPlanningContext>(
      bundle->scene, group_name, scene_mutex);
  bundle->clock = std::make_unique<WallClock>();
  bundle->execution_state_sink =
      std::make_shared<RoboPlanExecutionStateSink>(bundle->scene, scene_mutex);
  bundle->execution = std::make_unique<ros2_exec::Ros2ControlExecutionBackend>(
      bundle->execution_state_sink, group_name, *bundle->clock, ros_config);
  bundle->scheduler = std::make_unique<SimpleScheduler>();
  bundle->cache = std::make_unique<core::TrajectoryCache>();

  const auto policy = kernel_config.baseline_collision_policy_snapshot.value_or(
      default_collision_policy_snapshot());
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
  bundle->kernel = std::make_unique<core::ExecutionKernel>(
      *bundle->planner, *bundle->context, *bundle->execution,
      *bundle->clock, *bundle->scheduler, *bundle->cache,
      resolved_kernel_config);
  bundle->kernel->set_collision_validation_policy_snapshot(policy);
  bundle->kernel->set_collision_validation_pipeline(
      *bundle->context,
      bundle->collision_validation_engines);
  bundle->event_collector.subscribe(*bundle->kernel);
  return bundle;
}

}  // namespace manyforge::roboplan_backend
