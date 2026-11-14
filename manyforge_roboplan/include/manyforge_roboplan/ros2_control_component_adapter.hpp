#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

namespace manyforge::roboplan_backend {

/// The ros2_control lane adapter: one ros2_control component
/// (an arm, or a tool's gripper) over its runtime handle. Mock runs it as it
/// is; a simulator or vendor adapter may compose it. State comes from the
/// controller manager and the controllers; goal identity, binding epochs and
/// the controller-manager publisher checks stay inside the handle. Trust the
/// controller: no timers, checks or retries of its own.
class Ros2ControlComponentAdapter final : public core::IComponentAdapter {
public:
  /// The component's own streams as the host's stream registry reports them.
  using StreamsFn = std::function<std::vector<core::ComponentStream>(std::int64_t now_ns)>;

  /// `reset_phase`: the host's part of a `reset_position` (the write's
  /// acknowledgement and the commanded-state projection); with it and the
  /// handle's writer target (Mock, Isaac) the adapter declares
  /// `reset_position`.
  /// `simulated`: the component runs in a simulator (stale streams are
  /// simulator state, `Ros2ControlObservation::simulated`).
  /// `lifecycle_command_timeout`: the component's compiled lifecycle command
  /// time; each prepare, enable, disable and recover ends by it.
  /// `goal_result_bound`: an arm's lane bound for a goal's result after its
  /// expected end (`hard_stop_completion_timeout_s`): an open goal (the
  /// Stop's included) still without its result then makes the activity
  /// `unknown`.
  Ros2ControlComponentAdapter(std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle,
                              std::chrono::nanoseconds lifecycle_command_timeout,
                              MockResetPositionPhaseCallback reset_phase = {},
                              StreamsFn streams = {}, bool simulated = false,
                              std::optional<std::chrono::nanoseconds> goal_result_bound = {});

  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override;
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token stop) override;
  [[nodiscard]] std::vector<core::Capability> capabilities() const override;

private:
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle_;
  std::chrono::nanoseconds lifecycle_command_timeout_;
  MockResetPositionPhaseCallback reset_phase_;
  StreamsFn streams_;
  bool simulated_;
  std::optional<std::chrono::nanoseconds> goal_result_bound_;
};

/// The component adapter of a ros2_control lane family without vendor steps
/// (Mock, Isaac): the lane adapter, plus what an xArm machine does with its
/// driver. Prepare and Recover with an exited launch (`launch_exited`) start
/// it again through the host's driver-restart entry (`restart_driver`), then
/// run Prepare's step on the new launch (its controllers spawned and loaded,
/// within the lifecycle command time) and, for Recover, Recover's own step.
/// The component ends prepared: nothing is enabled and nothing moves. A
/// failed restart is the action's failure with its typed error; the exit
/// fault stays and the action can be tried again. Without a restart entry it
/// is the lane adapter.
class Ros2ControlDriverRestartAdapter final : public core::IComponentAdapter {
public:
  Ros2ControlDriverRestartAdapter(std::shared_ptr<core::IComponentAdapter> lane,
                                  std::function<bool()> launch_exited,
                                  std::function<core::Result<void>(std::stop_token)> restart_driver);

  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override;
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token stop) override;
  [[nodiscard]] std::vector<core::Capability> capabilities() const override;

private:
  std::shared_ptr<core::IComponentAdapter> lane_;
  std::function<bool()> launch_exited_;
  std::function<core::Result<void>(std::stop_token)> restart_driver_;
};

/// One primitive's outcome as an action result: succeeded; `unknown` when
/// its effect is unknown (never resent); otherwise failed with its typed
/// error. The coordinator stamps the times.
[[nodiscard]] core::ActionResult
ros2_control_action_result(core::ComponentAction action,
                           const RetainedRos2ControlSwitchResult &result);

} // namespace manyforge::roboplan_backend
