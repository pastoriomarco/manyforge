#include <manyforge_roboplan/simulated_plc_launch.hpp>

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace manyforge::roboplan_backend {
namespace {

[[noreturn]] void fail(const std::string &message) {
  throw ControllerLaunchProcessError("simulated PLC launch: " + message);
}

void sort_unique(std::vector<std::string> &values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

} // namespace

std::filesystem::path
resolve_simulated_plc_config_dir(const char *config_dir_env,
                                 const char *state_root_env) {
  if (config_dir_env != nullptr && *config_dir_env != '\0')
    return std::filesystem::path(config_dir_env);
  const std::filesystem::path root =
      state_root_env != nullptr && *state_root_env != '\0'
          ? std::filesystem::path(state_root_env)
          : std::filesystem::path(std::string(kWorkbenchStateRootDefault));
  return root / "sim_plc";
}

std::filesystem::path simulated_plc_config_dir_from_environment() {
  return resolve_simulated_plc_config_dir(
      std::getenv(std::string(kSimulatedPlcConfigDirEnv).c_str()),
      std::getenv(std::string(kWorkbenchStateRootEnv).c_str()));
}

std::optional<SimulatedPlcPlan>
plan_simulated_plc(const bool simulated_unit, std::string device_id,
                   const std::vector<SimulatedPlcRow> &rows,
                   std::filesystem::path config_dir) {
  // Only a simulated unit ever gets a simulated PLC.
  if (!simulated_unit)
    return std::nullopt;
  SimulatedPlcPlan plan;
  for (const auto &row : rows) {
    if (row.profile == "mock" || row.profile == "mock_output")
      continue;
    if (row.profile == "ros_bool" && row.topic && !row.topic->empty()) {
      plan.subscribed_topics.push_back(*row.topic);
    } else if (row.profile == "ros_set_bool" && row.service &&
               !row.service->empty()) {
      plan.served_services.push_back(*row.service);
    } else {
      // Robot-provided or other non-generic I/O: the robot's own peer serves it.
      return std::nullopt;
    }
  }
  if (plan.subscribed_topics.empty() && plan.served_services.empty())
    return std::nullopt;
  if (device_id.empty() || config_dir.empty())
    fail("a simulated PLC needs a device and a config directory");
  sort_unique(plan.subscribed_topics);
  sort_unique(plan.served_services);
  plan.device_id = std::move(device_id);
  plan.config_dir = std::move(config_dir);
  return plan;
}

ControllerLaunchCommand
simulated_plc_launch_command(const SimulatedPlcPlan &plan) {
  if (plan.device_id.empty() || plan.config_dir.empty() ||
      (plan.subscribed_topics.empty() && plan.served_services.empty()))
    fail("plan is incomplete");
  const auto config_dir = plan.config_dir.string();
  for (const auto *value : {&plan.device_id, &config_dir})
    if (value->find_first_of(" \t\n\r") != std::string::npos)
      fail("launch value contains whitespace: " + *value);
  ControllerLaunchCommand command;
  command.controllable_component_id = plan.device_id;
  command.lane = ControllerLaunchLane::kSimulatedPlc;
  command.argv = {"ros2",
                  "launch",
                  "manyforge_ros_bringup",
                  "sim_plc.launch.py",
                  "device:=" + plan.device_id,
                  "config_dir:=" + config_dir};
  std::ostringstream identity;
  identity << "sim_plc\n" << plan.device_id << '\n' << config_dir << '\n';
  for (const auto &topic : plan.subscribed_topics)
    identity << "topic " << topic << '\n';
  for (const auto &service : plan.served_services)
    identity << "service " << service << '\n';
  command.identity = identity.str();
  // An unchanged simulated PLC keeps its process and I/O values.
  command.keep_across_apply = true;
  return command;
}

bool simulated_plc_launch_survives(
    ControllerLaunchProcess *const process,
    const std::optional<SimulatedPlcPlan> &candidate) {
  return process != nullptr && candidate.has_value() &&
         process->command().lane == ControllerLaunchLane::kSimulatedPlc &&
         owned_launch_live(process, false, false) &&
         process->command().identity ==
             simulated_plc_launch_command(*candidate).identity;
}

} // namespace manyforge::roboplan_backend
