// The generic simulator adapter family `simulation_interfaces` (CONTRACTS §6,
// §14): one library, registered in the ament resource index
// `manyforge_component_adapters`. A simulator component's model row names it
// (`adapter: simulation_interfaces`) and carries its services and timing.

#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/simulator_component_adapter.hpp>

#include "ros_simulation_control.hpp"

namespace {

manyforge::roboplan_backend::ComponentAdapters make_simulator_adapter(
    const manyforge::roboplan_backend::ComponentAdapterContext &context) {
  using namespace manyforge::roboplan_backend;
  return {std::make_shared<SimulatorComponentAdapter>(std::make_shared<RosSimulationControl>(
              context.node, simulation_services_from_row(context.row))),
          {},
          std::nullopt,
          std::nullopt};
}

// A simulator runs no ros2_control lane: no launch contribution.
constexpr manyforge::roboplan_backend::ComponentAdapterFamily kFamily{
    "simulation_interfaces", &make_simulator_adapter, nullptr, nullptr, nullptr};

}  // namespace

extern "C" const manyforge::roboplan_backend::ComponentAdapterFamily *
manyforge_component_adapter_family() {
  return &kFamily;
}
