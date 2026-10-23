#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <manyforge_roboplan/simulated_plc_launch.hpp>

#include "component_runtime_pins.hpp"

namespace rclcpp {
class Context;
}

namespace manyforge::roboplan_backend::host_detail {

/// Endpoints of `plan` still missing when the wait ended; empty when ready.
struct SimulatedPlcReadiness final {
  std::vector<std::string> missing_services;
  std::vector<std::string> missing_topic_samples;
  [[nodiscard]] bool ready() const noexcept {
    return missing_services.empty() && missing_topic_samples.empty();
  }
};

/// Bounded Apply-time wait (design §4): on the host's own ROS Context, every
/// served SetBool service is ready for a client and the first Bool sample has
/// arrived on every subscribed topic. A temporary probe node and executor
/// live only for this call; no state is retained. `abandon` (optional) ends
/// the wait early, e.g. once the supervised process has exited.
[[nodiscard]] SimulatedPlcReadiness
WaitForSimulatedPlcEndpoints(const std::shared_ptr<rclcpp::Context> &context,
                             const SimulatedPlcPlan &plan,
                             std::chrono::milliseconds timeout,
                             const std::function<bool()> &abandon = {});

using SimulatedPlcWait = std::function<SimulatedPlcReadiness(
    const std::shared_ptr<ControllerLaunchProcess> &)>;

/// Start or keep one Apply candidate's simulated PLC (supervisor keep/restart
/// rules), then wait. `borrowed_drivers`: the drivers belong to the installed
/// owner (exact retained component); then only `kKeep` of exactly
/// `installed_process` is accepted and the drivers are never touched, so a
/// failed candidate cannot clear the installed crash fault. Otherwise the
/// drivers are this candidate's own and observe the process's exit once ready.
/// Any refusal discards a freshly started process (a kept one never) and
/// throws `simulated_plc_not_ready`.
[[nodiscard]] ControllerLaunchSupervisor::Ensured StartSimulatedPlcCandidate(
    ControllerLaunchSupervisor &supervisor, const SimulatedPlcPlan &plan,
    const std::filesystem::path &artifacts_root, const std::string &context,
    const IoProviderDriverGroup &drivers, bool borrowed_drivers,
    const std::shared_ptr<ControllerLaunchProcess> &installed_process,
    const SimulatedPlcWait &wait);

} // namespace manyforge::roboplan_backend::host_detail
