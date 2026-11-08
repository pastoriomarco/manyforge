#pragma once

#include <cstdint>
#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_core/types/component_state.hpp>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <vector>

namespace manyforge::roboplan_backend {

/// The generic simulator interface, on the ROS 2
/// `simulation_interfaces` standard (Isaac Sim, Gazebo and O3DE implement
/// it). Nothing here knows a particular simulator: service names and timing
/// come from the simulator component's lane row.

/// The standard's simulation state.
enum class SimulationState : std::uint8_t { kStopped = 0, kPlaying, kPaused };

/// One simulator request's outcome: `succeeded`, `failed` with a typed error,
/// or `unknown` when no response came back.
struct SimulationCall final {
  core::ActionOutcome outcome{core::ActionOutcome::kUnknown};
  std::optional<core::ActionError> error;
};

/// The simulator, ROS-free (the ROS side is RosSimulationControl).
class ISimulationControl {
 public:
  virtual ~ISimulationControl() = default;
  /// The state the simulator last reported; empty while it doesn't answer.
  [[nodiscard]] virtual std::optional<SimulationState> Observed() const = 0;
  /// Reads the state now (GetSimulationState); a success updates Observed().
  [[nodiscard]] virtual SimulationCall Read(std::stop_token stop) = 0;
  /// SetSimulationState; "already in the target state" is a success.
  [[nodiscard]] virtual SimulationCall Set(SimulationState state, std::stop_token stop) = 0;
  /// ResetSimulation.
  [[nodiscard]] virtual SimulationCall Reset(std::stop_token stop) = 0;
};

/// Reasons of a reachable simulator that isn't playing (`prepared`, never a
/// fault) and of one that doesn't answer (`offline`).
inline constexpr std::string_view kSimulationPausedReason = "simulation_paused";
inline constexpr std::string_view kSimulationStoppedReason = "simulation_stopped";
inline constexpr std::string_view kSimulatorUnreachableReason = "simulator_unreachable";

/// The simulator component's state: `playing` = `enabled`; `paused` and
/// `stopped` = `prepared` with their reason; unreachable = `offline`.
/// An unanswered request remains an `unknown` action result; state is derived
/// only from the simulator's observation.
[[nodiscard]] core::ComponentState SimulatorComponentState(
    const std::optional<SimulationState> &observed, std::int64_t now_ns);

/// The simulator component's adapter. `prepare` and
/// `recover` read the state; `enable` plays, `disable` pauses,
/// `sim_reset` resets. A request without a response keeps its `unknown`
/// action result and is never resent.
class SimulatorComponentAdapter final : public core::IComponentAdapter {
 public:
  explicit SimulatorComponentAdapter(std::shared_ptr<ISimulationControl> control);

  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override;
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token stop) override;
  [[nodiscard]] std::vector<core::Capability> capabilities() const override;

 private:
  [[nodiscard]] SimulationCall Set(SimulationState state, const std::stop_token &stop);

  std::shared_ptr<ISimulationControl> control_;
};

}  // namespace manyforge::roboplan_backend
