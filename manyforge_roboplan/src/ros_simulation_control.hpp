#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <manyforge_roboplan/launch_contribution.hpp>
#include <manyforge_roboplan/adapter_transport_consumer.hpp>
#include <manyforge_roboplan/simulator_component_adapter.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <rclcpp/rclcpp.hpp>
#include <simulation_interfaces/srv/get_simulation_state.hpp>
#include <simulation_interfaces/srv/reset_simulation.hpp>
#include <simulation_interfaces/srv/set_simulation_state.hpp>
#include <string>

namespace manyforge::ros2_control_execution {
class Ros2TransportResources;
}

namespace manyforge::roboplan_backend {

/// The simulator's `simulation_interfaces` services and timing, from the
/// simulator component's lane row (`get_state_service`, `set_state_service`,
/// `reset_service`, `response_timeout_s`, `state_poll_period_s`).
struct SimulationServices final {
  std::string get_state;
  std::string set_state;
  std::string reset;
  std::chrono::nanoseconds response_timeout{};
  std::chrono::nanoseconds state_poll_period{};
};

/// Reads SimulationServices from a lane row; throws std::invalid_argument
/// for a missing or invalid key.
[[nodiscard]] SimulationServices
simulation_services_from_row(const ModelLaneRow &row);

/// ISimulationControl over the standard's services on `node`, which the host
/// spins. Observed() comes from a GetSimulationState read every poll period
/// (the standard publishes no state topic); a read unanswered within the
/// response timeout, or a service that isn't there, reads as unreachable.
class RosSimulationControl final : public ISimulationControl,
                                   public AdapterTransportConsumer {
public:
  RosSimulationControl(
      rclcpp::Node::SharedPtr node, SimulationServices services,
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner =
          {});
  ~RosSimulationControl() override;

  [[nodiscard]] std::optional<SimulationState> Observed() const override;
  [[nodiscard]] SimulationCall Read(std::stop_token stop) override;
  [[nodiscard]] SimulationCall Set(SimulationState state,
                                   std::stop_token stop) override;
  [[nodiscard]] SimulationCall Reset(std::stop_token stop) override;

  std::shared_ptr<void> Stage(const std::shared_ptr<Owner> &expected,
                              const std::shared_ptr<Owner> &replacement,
                              Gate active) override;
  bool Validate(const std::shared_ptr<void> &staged) const override;
  void Commit(const std::shared_ptr<void> &staged) noexcept override;

private:
  using GetState = simulation_interfaces::srv::GetSimulationState;
  using SetState = simulation_interfaces::srv::SetSimulationState;
  using ResetSimulation = simulation_interfaces::srv::ResetSimulation;

  struct State;
  struct Rebind;
  std::shared_ptr<State> CreateState(
      rclcpp::Node::SharedPtr node,
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner,
      std::shared_ptr<std::atomic<bool>> active);
  std::shared_ptr<State> Snapshot() const;
  SimulationServices services_;
  mutable std::mutex state_mutex_;
  std::shared_ptr<State> state_;
};

} // namespace manyforge::roboplan_backend
