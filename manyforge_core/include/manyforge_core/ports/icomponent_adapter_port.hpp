#pragma once

#include <cstdint>
#include <manyforge_core/types/component_state.hpp>
#include <stop_token>
#include <vector>

namespace manyforge::core {

/// CONTRACTS §6: the boundary every lane and vendor adapter implements, one
/// per physical connection. ROS-free; the component coordinator depends on it.
///
/// Trust the controller: no extra timers, checks or retries of the adapter's
/// own; it reports what the device or controller reports.
class IComponentAdapter {
 public:
  virtual ~IComponentAdapter() = default;

  /// Pure read of the latest observations: lifecycle, fault, reason, activity
  /// and streams. The coordinator fills `component_id`, `capabilities`,
  /// `action` and `last_action`.
  [[nodiscard]] virtual ComponentState State(std::int64_t now_ns) const = 0;

  /// Blocking: runs the vendor sequence for `action` and returns its outcome
  /// and, unless it succeeded, a typed error. An action whose effect is
  /// unknown ends `unknown` and is never resent. `stop` is requested when a
  /// Stop interrupts the action. The coordinator stamps `action` and the times.
  ///
  /// An action's result is the component's state: Run returns `succeeded`
  /// only once State() already shows it (prepare/reboot → prepared or enabled,
  /// recover → neither fault nor offline, enable → enabled, disable →
  /// prepared, power_off → offline). Where the device reports with a lag, Run
  /// waits for that report within the lifecycle command time; a report that
  /// doesn't come is `component_action_result_not_observed`. The coordinator
  /// records a succeeded action whose state doesn't show it as that failure.
  [[nodiscard]] virtual ActionResult Run(ComponentAction action, std::stop_token stop) = 0;

  /// The capabilities this adapter declares (CONTRACTS §3).
  [[nodiscard]] virtual std::vector<Capability> capabilities() const = 0;
};

}  // namespace manyforge::core
