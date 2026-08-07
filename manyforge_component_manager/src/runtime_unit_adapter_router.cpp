#include "manyforge_component_manager/runtime_unit_adapter_router.hpp"

#include <algorithm>
#include <utility>

namespace manyforge::component_manager {

RuntimeUnitAdapterRouter::RuntimeUnitAdapterRouter(
    std::vector<RuntimeUnitAdapterRoute> routes)
    : routes_(std::move(routes)) {
  if (routes_.empty()) {
    throw std::invalid_argument("runtime adapter router has no routes");
  }
  std::sort(routes_.begin(), routes_.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.process_adapter_contract_id <
                     rhs.process_adapter_contract_id;
            });
  for (std::size_t index = 0U; index < routes_.size(); ++index) {
    const auto &route = routes_[index];
    if (route.process_adapter_contract_id.empty() ||
        route.process_port == nullptr || route.progress_port == nullptr ||
        (index > 0U && routes_[index - 1U].process_adapter_contract_id ==
                           route.process_adapter_contract_id)) {
      throw std::invalid_argument(
          "runtime adapter router has an invalid or duplicate route");
    }
  }
}

const RuntimeUnitAdapterRoute &RuntimeUnitAdapterRouter::Select(
    const RuntimeUnitConvergenceQueryDTO &query) const {
  if (!query.valid()) {
    throw RuntimeUnitAdapterRouterError(
        "runtime adapter router received an invalid convergence query");
  }
  const auto found = std::lower_bound(
      routes_.begin(), routes_.end(), query.plan.process_adapter_contract_id,
      [](const RuntimeUnitAdapterRoute &route, const std::string &contract_id) {
        return route.process_adapter_contract_id < contract_id;
      });
  if (found == routes_.end() || found->process_adapter_contract_id !=
                                    query.plan.process_adapter_contract_id) {
    throw RuntimeUnitAdapterRouterError(
        "runtime plan selects no build-qualified process adapter");
  }
  return *found;
}

ProcessPresenceObservation RuntimeUnitAdapterRouter::ObserveProcess(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return Select(query).process_port->ObserveProcess(query);
}

ProcessPresenceObservation RuntimeUnitAdapterRouter::EnsureRunning(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return Select(query).process_port->EnsureRunning(query);
}

ProcessPresenceObservation RuntimeUnitAdapterRouter::EnsureStopped(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return Select(query).process_port->EnsureStopped(query);
}

std::optional<RuntimeUnitSourceReport>
RuntimeUnitAdapterRouter::ObserveProgress(
    const RuntimeUnitConvergenceQueryDTO &query,
    const manyforge::core::RuntimeEpoch &runtime_epoch) {
  return Select(query).progress_port->ObserveProgress(query, runtime_epoch);
}

} // namespace manyforge::component_manager
