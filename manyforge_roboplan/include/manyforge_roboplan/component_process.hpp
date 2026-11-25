#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::roboplan_backend {

/// Component process: the host's process launcher for a
/// component that runs its own process (e.g. a sensor bridge), under the
/// host's process supervisor like every other component process. ROS-free.

/// `ros2 launch <package> <launch_file> <arguments...>` (`name:=value`).
struct ComponentProcessCommand final {
  std::string package;
  std::string launch_file;
  std::vector<std::string> arguments;
};

/// One running component process. Its owner stops it with `stop()` on the
/// thread that replaces or ends it; dropping a copy (e.g. a State() reader's)
/// then only releases it. Destroying one that was never stopped stops it.
class IComponentProcess {
 public:
  virtual ~IComponentProcess() = default;
  /// Stops the process gracefully, with the supervisor's existing grace.
  /// Idempotent: returns at once when it was already stopped.
  virtual void stop() = 0;
  /// The exit reason once the process has exited; empty while it runs.
  [[nodiscard]] virtual std::optional<std::string> exited() = 0;
  /// The last lines of its retained output for a fault's `output_tail`
  /// (bounded, memory only); empty when none.
  [[nodiscard]] virtual std::string output_tail() const = 0;
};

/// Starts the process; throws `std::runtime_error` when it can't start.
using ComponentProcessLauncher =
    std::function<std::unique_ptr<IComponentProcess>(const ComponentProcessCommand &)>;

}  // namespace manyforge::roboplan_backend
