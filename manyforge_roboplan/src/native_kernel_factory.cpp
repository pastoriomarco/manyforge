#include <manyforge_roboplan/native_kernel_factory.hpp>

#include <roboplan_example_models/resources.hpp>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace manyforge::roboplan_backend {

// --- WallClock ---

core::TimePoint WallClock::now() const {
  auto tp = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             tp.time_since_epoch())
      .count();
}

// --- SimpleScheduler ---

core::ScheduleHandle SimpleScheduler::schedule(
    core::TimePoint /*deadline*/, std::function<void()> callback) {
  // v0.0: fire immediately (no deferred scheduling needed for sync kernel)
  auto handle = next_handle_.fetch_add(1);
  if (callback) callback();
  return handle;
}

void SimpleScheduler::cancel(core::ScheduleHandle /*handle*/) {
  // No-op in v0.0 sync mode
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
    const std::vector<std::string>& package_paths) {

  auto bundle = std::make_shared<NativeKernelBundle>();

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
  bundle->planner = std::make_unique<RoboPlanPlannerBackend>(
      bundle->scene, bundle->rrt, bundle->shortcutter, bundle->toppra, bundle->ik);
  bundle->context = std::make_unique<RoboPlanPlanningContext>(
      bundle->scene, group_name);
  bundle->execution = std::make_unique<RoboPlanExecutionBackend>(bundle->scene);
  bundle->clock = std::make_unique<WallClock>();
  bundle->scheduler = std::make_unique<SimpleScheduler>();
  bundle->cache = std::make_unique<core::TrajectoryCache>();

  // Wire kernel — references to owned components
  bundle->kernel = std::make_unique<core::ExecutionKernel>(
      *bundle->planner, *bundle->context, *bundle->execution,
      *bundle->clock, *bundle->scheduler, *bundle->cache);

  return bundle;
}

// --- Factory (convenience, uses example_models) ---

std::shared_ptr<NativeKernelBundle> create_native_kernel(
    const std::string& model_name,
    const std::string& group_name) {

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
                               {pkg_share.string()});
}

}  // namespace manyforge::roboplan_backend
