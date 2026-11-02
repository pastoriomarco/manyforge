#pragma once

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/ports/iexecution_port.hpp>

namespace manyforge::core {

/// One actual fresh-trip spring return point. The owning stage attempt retains
/// its existing Stop, operation, plan and stage identities. This value proves
/// neither Stop feasibility nor arrival; successor dispatch still requires the
/// same Stop certificate and exact local settlement.
struct SpringBackReturnPointDTO final {
  ExecutionGoalKey source_goal;
  ExecutionBindingEpoch binding_epoch{0U};
  ExecutionControllerSampleDTO trip;
  SoftOnPathResolvedInitialStateDTO initial;
  /// Qualified by the participant's existing delivery-clock mapping. A raw
  /// helper point alone makes no claim about the canonical prepared clock.
  std::optional<double> canonical_return_path_time_s{};
};

/// Scoped to one synchronous stage call; providers must not retain the
/// observer. It only transfers an immutable point, never plans or dispatches.
class IStopStageSpringReturnObserver {
 public:
  virtual ~IStopStageSpringReturnObserver() = default;
  [[nodiscard]] virtual bool SpringReturnPointReady(
      const SpringBackReturnPointDTO& point) noexcept = 0;
};

}  // namespace manyforge::core
