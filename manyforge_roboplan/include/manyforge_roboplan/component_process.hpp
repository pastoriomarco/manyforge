#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::roboplan_backend {

/// CONTRACTS §6 component process: the host's process launcher for a
/// component that runs its own process (e.g. a sensor bridge), under the
/// host's process supervisor like every other component process. ROS-free.

/// `ros2 launch <package> <launch_file> <arguments...>` (`name:=value`).
struct ComponentProcessCommand final {
  std::string package;
  std::string launch_file;
  std::vector<std::string> arguments;
};

/// One running component process; destroying it stops the process.
class IComponentProcess {
 public:
  virtual ~IComponentProcess() = default;
  /// The exit reason once the process has exited; empty while it runs.
  [[nodiscard]] virtual std::optional<std::string> exited() = 0;
};

/// Starts the process; throws `std::runtime_error` when it can't start.
using ComponentProcessLauncher =
    std::function<std::unique_ptr<IComponentProcess>(const ComponentProcessCommand &)>;

}  // namespace manyforge::roboplan_backend
