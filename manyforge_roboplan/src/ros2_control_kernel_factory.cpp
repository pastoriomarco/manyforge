#include <manyforge_roboplan/ros2_control_kernel_factory.hpp>

#include <algorithm>
#include <filesystem>
#include <mutex>

#include <manyforge_core/collision/collision_resource_manager.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_roboplan/roboplan_execution_state_sink.hpp>
#include <manyforge_roboplan/roboplan_focused_adapters.hpp>
#include <manyforge_roboplan/roboplan_planner_options.hpp>
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
    const core::ExecutionKernelConfig& kernel_config,
    const std::string& logical_robot_id) {
  auto bundle = std::make_shared<NativeKernelBundle>();
  const std::string robot_id =
      logical_robot_id.empty() ? model_name : logical_robot_id;
  bundle->robot_id = robot_id;
  bundle->group_name = group_name;

  std::vector<std::filesystem::path> pkg_paths;
  for (const auto& path : package_paths) {
    pkg_paths.emplace_back(path);
  }

  bundle->scene = std::make_shared<roboplan::Scene>(
      model_name, std::filesystem::path(urdf_path),
      std::filesystem::path(srdf_path), pkg_paths);

  const auto rrt_opts = production_rrt_options(
      group_name, kernel_config.planner_max_planning_time_s);
  bundle->rrt = std::make_shared<roboplan::RRT>(bundle->scene, rrt_opts);
  bundle->shortcutter = std::make_shared<roboplan::PathShortcutter>(
      bundle->scene, group_name);
  bundle->toppra = std::make_shared<roboplan::PathParameterizerTOPPRA>(
      bundle->scene, group_name);

  const auto ik_opts = production_planning_ik_options(group_name);
  bundle->ik = std::make_shared<roboplan::SimpleIk>(bundle->scene, ik_opts);
  const auto single_shot_ik_opts = production_single_shot_ik_options(
      group_name, kernel_config.single_shot_ik_timeout_s);
  bundle->single_shot_ik =
      std::make_shared<roboplan::SimpleIk>(bundle->scene, single_shot_ik_opts);

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
  bundle->scheduler = std::make_unique<SimpleScheduler>();
  bundle->execution_state_sink =
      std::make_shared<RoboPlanExecutionStateSink>(
          bundle->scene, bundle->scene_mutex);
  auto ros_execution =
      std::make_unique<ros2_exec::Ros2ControlExecutionBackend>(
      bundle->execution_state_sink, group_name, *bundle->clock,
      *bundle->scheduler, ros_config);
  auto* ros_execution_ptr = ros_execution.get();
  bundle->gripper_command_backend = ros_execution_ptr;
  bundle->execution = std::move(ros_execution);
  const auto* observed_publication =
      bundle->execution->observed_robot_state_publication();
  if (observed_publication == nullptr) {
    throw std::runtime_error(
        "ros2_control execution backend did not publish observed robot state");
  }
  bundle->observed_state_provider =
      std::make_unique<core::PublishedObservedRobotStateProvider>(
          *observed_publication,
          ros_config.execution_state_source_mode == "commanded_sim" ||
                  ros_config.execution_state_source_mode == "authored_preview"
              ? core::ObservedRobotStateAuthorityProfile::kCommandedSimulation
              : core::ObservedRobotStateAuthorityProfile::kMeasured,
          ros_execution_ptr->authored_state_authority_epoch());
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
          "ros2_control kernel collision resources are not runtime-safe: " +
          admission.resource_conflict_reason);
    }
  }

  core::ExecutionKernelConfig resolved_kernel_config = kernel_config;
  resolved_kernel_config.validation_collision_workers = collision_worker_count;
  const bool command_authoritative =
      ros_config.execution_state_source_mode == "commanded_sim" ||
      ros_config.execution_state_source_mode == "authored_preview";
  // Recovery authority follows the deployment's ROS state-authority profile.
  // Command/authored simulation is explicitly qualified but is not measured;
  // real observed ROS profiles retain the fail-closed measured requirement.
  resolved_kernel_config.recovery_require_measured_observation =
      !command_authoritative;
  if (resolved_kernel_config.recovery_observed_joint_order_token.empty()) {
    const auto* observed =
        bundle->execution->observed_robot_state_publication();
    if (observed != nullptr) {
      resolved_kernel_config.recovery_observed_joint_order_token =
          observed->joint_order_token();
    }
  }
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
  bundle->event_collector.subscribe(*bundle->kernel);
  return bundle;
}

}  // namespace manyforge::roboplan_backend
