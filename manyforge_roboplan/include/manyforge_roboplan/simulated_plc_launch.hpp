#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_roboplan/controller_launch_process.hpp>

namespace manyforge::roboplan_backend {

/// sim-plc-peer-design-02/03: a ManyForge-owned simulated PLC for one generic
/// ROS I/O device in a simulated unit (no hosted real component). The PLC owns its own
/// settings file (`<config_dir>/<device>.yaml`); ManyForge passes only the
/// device name and the config directory. ROS-free.

/// Host environment override of the simulated PLC settings directory.
inline constexpr std::string_view kSimulatedPlcConfigDirEnv =
    "MANYFORGE_SIM_PLC_CONFIG_DIR";
/// Existing workbench runtime state root (mounted at this container path by
/// `scripts/compose-dev.sh`; `scripts/dev-workbench.py` honours the env).
inline constexpr std::string_view kWorkbenchStateRootEnv =
    "MANYFORGE_DEV_STATE_ROOT";
inline constexpr std::string_view kWorkbenchStateRootDefault =
    "/tmp/manyforge-state";

/// One authored I/O row of the device, as the host's retained catalog has it.
struct SimulatedPlcRow final {
  std::string profile;
  std::optional<std::string> topic;
  std::optional<std::string> service;
};

/// What ManyForge launches and then waits for. Endpoint names come only from
/// the Deployment rows (sorted, unique); the PLC's own file is not read here.
struct SimulatedPlcPlan final {
  std::string device_id;
  std::filesystem::path config_dir;
  /// `ros_bool` input rows: Bool topics the PLC publishes.
  std::vector<std::string> subscribed_topics;
  /// `ros_set_bool` output rows: SetBool services the PLC serves.
  std::vector<std::string> served_services;

  friend bool operator==(const SimulatedPlcPlan &,
                         const SimulatedPlcPlan &) = default;
};

/// `MANYFORGE_SIM_PLC_CONFIG_DIR` when set and nonempty, otherwise
/// `<MANYFORGE_DEV_STATE_ROOT or /tmp/manyforge-state>/sim_plc`. Arguments are
/// the raw environment values (nullptr when unset).
[[nodiscard]] std::filesystem::path
resolve_simulated_plc_config_dir(const char *config_dir_env,
                                 const char *state_root_env);
/// The same, reading this process's environment.
[[nodiscard]] std::filesystem::path simulated_plc_config_dir_from_environment();

/// A plan only for a simulated unit (a ROS unit hosting no component with
/// measured, i.e. real, state authority) whose device rows use the generic
/// `ros_bool` / `ros_set_bool` profiles (Mock rows are ignored). Any
/// robot-provided row (`ur_io_states`, `ur_set_io`, `ros2_control_gpio`, or
/// another non-generic profile) or no generic ROS row yields no plan.
[[nodiscard]] std::optional<SimulatedPlcPlan>
plan_simulated_plc(bool simulated_unit, std::string device_id,
                   const std::vector<SimulatedPlcRow> &rows,
                   std::filesystem::path config_dir);

/// `ros2 launch manyforge_ros_bringup sim_plc.launch.py device:=<id>
/// config_dir:=<dir>`, kept across Apply on equal identity. The identity is the
/// device, the config directory and the sorted endpoint names, never the
/// settings file contents.
[[nodiscard]] ControllerLaunchCommand
simulated_plc_launch_command(const SimulatedPlcPlan &plan);

/// D4, like external hardware: a running simulated PLC process outlives a
/// rebuild of its consumer device (changed device rows) exactly when the
/// candidate still plans a PLC with the same launch identity. Absent plan
/// (device removed, Close, not simulated) or any identity change: retire.
[[nodiscard]] bool
simulated_plc_launch_survives(ControllerLaunchProcess *process,
                              const std::optional<SimulatedPlcPlan> &candidate);

} // namespace manyforge::roboplan_backend
