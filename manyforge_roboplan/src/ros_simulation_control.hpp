#pragma once

#include <chrono>
#include <cstdint>
#include <manyforge_roboplan/launch_contribution.hpp>
#include <manyforge_roboplan/simulator_component_adapter.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <rclcpp/rclcpp.hpp>
#include <simulation_interfaces/srv/get_simulation_state.hpp>
#include <simulation_interfaces/srv/reset_simulation.hpp>
#include <simulation_interfaces/srv/set_simulation_state.hpp>
#include <string>

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
[[nodiscard]] SimulationServices simulation_services_from_row(const ModelLaneRow &row);

/// ISimulationControl over the standard's services on `node`, which the host
/// spins. Observed() comes from a GetSimulationState read every poll period
/// (the standard publishes no state topic); a read unanswered within the
/// response timeout, or a service that isn't there, reads as unreachable.
class RosSimulationControl final : public ISimulationControl {
 public:
  RosSimulationControl(rclcpp::Node::SharedPtr node, SimulationServices services);
  ~RosSimulationControl() override;

  [[nodiscard]] std::optional<SimulationState> Observed() const override;
  [[nodiscard]] SimulationCall Read(std::stop_token stop) override;
  [[nodiscard]] SimulationCall Set(SimulationState state, std::stop_token stop) override;
  [[nodiscard]] SimulationCall Reset(std::stop_token stop) override;

 private:
  using GetState = simulation_interfaces::srv::GetSimulationState;
  using SetState = simulation_interfaces::srv::SetSimulationState;
  using ResetSimulation = simulation_interfaces::srv::ResetSimulation;

  void Poll();
  void Record(std::optional<SimulationState> state);

  rclcpp::Node::SharedPtr node_;
  SimulationServices services_;
  rclcpp::Client<GetState>::SharedPtr get_state_;
  rclcpp::Client<SetState>::SharedPtr set_state_;
  rclcpp::Client<ResetSimulation>::SharedPtr reset_;
  rclcpp::TimerBase::SharedPtr timer_;
  mutable std::mutex mutex_;
  std::optional<SimulationState> observed_;
  /// The poll read in flight and when it was sent.
  std::optional<std::int64_t> poll_request_;
  std::chrono::steady_clock::time_point poll_sent_;
};

}  // namespace manyforge::roboplan_backend
