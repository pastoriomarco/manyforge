#pragma once

#include <cstdint>
#include <functional>
#include <manyforge_core/component_coordinator.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_core/types/component_state.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {

/// The execution host's component runtime (CONTRACTS §1, §2, §5, §6). It owns
/// the core component coordinator for the whole cell, installed once with the
/// stable Kernel and kept across Apply. Every component state change goes to
/// the Kernel's cell latch and to the host's publication (event stream state
/// and the `components` read); a successful `enable` opens the component's
/// command routes in the Kernel before the action reports success.
class HostComponentRuntime final {
 public:
  struct Hooks final {
    /// The Kernel's feed (`observe_component_states`): every component of
    /// the cell, on every change. The Kernel derives the cell latch from it
    /// and starts a newly `prepared` component's `changed_outside` signals
    /// false.
    std::function<void(std::vector<core::ComponentState>)> observe_states;
    /// Runs inside a component's `enable` after its adapter succeeded:
    /// publishes the component's routes to the Kernel
    /// (`publish_runtime_route_configuration`). Its failure fails the action
    /// with its error.
    std::function<core::Result<void>(const std::string &component_id)> open_routes;
    /// The publication changed (wakes the event stream writers).
    std::function<void()> published;
  };

  /// One publication: every component's state in cell order, with a revision
  /// that grows on every change.
  struct Publication final {
    std::uint64_t revision{};
    std::vector<core::ComponentState> states;
  };

  HostComponentRuntime(core::IClock &clock, Hooks hooks);
  ~HostComponentRuntime();
  HostComponentRuntime(const HostComponentRuntime &) = delete;
  HostComponentRuntime &operator=(const HostComponentRuntime &) = delete;

  /// The components of the cell after an Apply, in cell order. A component
  /// whose adapter is unchanged keeps its action and `last_action`; a removed
  /// component no longer counts for the latch (R3). Returns the removed
  /// components' interrupted action threads: destroy them (join) after
  /// releasing the composition lock, which their actions may take.
  [[nodiscard]] std::vector<std::jthread> SetComponents(
      std::vector<core::CoordinatedComponent> components);

  /// CONTRACTS §2: starts `action`; the result arrives in `last_action`.
  [[nodiscard]] core::Result<core::ComponentActionInProgress> Submit(
      const std::string &component_id, core::ComponentAction action);

  /// A Stop was accepted: every component action in progress is interrupted.
  void InterruptActions();

  /// The host tick: reads every adapter and publishes on change.
  void Refresh();

  [[nodiscard]] Publication Read() const;

  /// The adapter the component runs on (not its route-opening wrapper), or
  /// null when the component isn't in the cell. Used for the adapter's own
  /// small interfaces, e.g. the output write of CONTRACTS §7.
  [[nodiscard]] std::shared_ptr<core::IComponentAdapter> Adapter(
      const std::string &component_id) const;

 private:
  class RouteOpeningAdapter;

  void Publish(const std::vector<core::ComponentState> &states);

  Hooks hooks_;
  mutable std::mutex mutex_;
  Publication publication_;
  /// The route-opening wrapper of each adapter, kept so that an adapter
  /// retained across Apply stays the same coordinated adapter.
  std::map<const core::IComponentAdapter *, std::shared_ptr<RouteOpeningAdapter>> wrappers_;
  /// Declared last: destroyed first, so no action outlives the hooks.
  core::ComponentCoordinator coordinator_;
};

/// `open_routes` (CONTRACTS §6, §15): whether the bundle compiles a route
/// for `component`, i.e. any compiled route names one of its control
/// bindings. A component without one (sensor, I/O, passive, simulator,
/// owner) has nothing to open.
[[nodiscard]] bool HasCompiledComponentRoutes(
    const std::vector<core::DesiredControlConfigurationDTO> &compiled_routes,
    const std::vector<core::ControlBindingDTO> &bindings,
    const core::ControllableComponentId &component);

}  // namespace manyforge::roboplan_backend::host_detail
