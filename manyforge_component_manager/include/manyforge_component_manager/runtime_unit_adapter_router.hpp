#pragma once

#include <manyforge_component_manager/runtime_convergence.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::component_manager {

class RuntimeUnitAdapterRouterError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// One closed build-qualified process-adapter route.
///
/// The contract ID selects an adapter family only. The selected adapter still
/// verifies the exact contract revision and complete retained product before
/// any process effect.
struct RuntimeUnitAdapterRoute final {
  std::string process_adapter_contract_id;
  IRuntimeUnitProcessPort *process_port{};
  IRuntimeUnitProgressPort *progress_port{};
};

/// Closed dispatcher used by the production Manager composition root.
///
/// It does not discover plugins or treat retained launch data as executable
/// input. An unregistered adapter contract rejects before delegation.
class RuntimeUnitAdapterRouter final : public IRuntimeUnitProcessPort,
                                       public IRuntimeUnitProgressPort {
public:
  explicit RuntimeUnitAdapterRouter(
      std::vector<RuntimeUnitAdapterRoute> routes);

  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] ProcessPresenceObservation
  EnsureStopped(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] std::optional<RuntimeUnitSourceReport>
  ObserveProgress(const RuntimeUnitConvergenceQueryDTO &query,
                  const manyforge::core::RuntimeEpoch &runtime_epoch) override;

private:
  [[nodiscard]] const RuntimeUnitAdapterRoute &
  Select(const RuntimeUnitConvergenceQueryDTO &query) const;

  std::vector<RuntimeUnitAdapterRoute> routes_;
};

} // namespace manyforge::component_manager
