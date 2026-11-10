#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/component_state.hpp>

namespace manyforge::roboplan_backend {

/// What a ros2_control component's controller manager and controllers report
/// right now (ROS-free; `RetainedRos2ControlRuntimeHandle::ReadObservation`
/// fills it). Nothing here is inferred by ManyForge: it is the controller's
/// own report.
struct Ros2ControlObservation final {
  /// The exact owner and task of a latched ROS owner stall, if any.
  std::optional<std::string> owner_stalled;
  /// The component's launch exited, or its ROS executor failed.
  std::optional<std::string> process_fault;
  /// A lane guard's fault awaiting Recover (the Isaac command-age guard).
  std::optional<std::string> guard_fault;
  /// A vendor tool-context call that may have changed the controller without
  /// confirmation; Recover ends it.
  std::optional<std::string> tool_context_fault;
  /// The controller manager on its `~/activity` topic: matched, and at least
  /// one sample received.
  bool controller_manager_present{};
  bool observed{};
  /// The component's controller (the arm's JTC, or a tool's gripper
  /// controller) and its hardware.
  bool controller_loaded{};
  bool controller_active{};
  bool hardware_active{};
  /// A gripper owner without a controller manager (an action bridge or a
  /// no-output owner): its readiness and whether it is enabled.
  std::optional<bool> local_owner_ready;
  bool local_owner_enabled{};
  /// The controller reports a goal (or a gripper command) not yet finished.
  bool goal_running{};
  /// An exact goal's required late cancellation could not be scheduled.
  bool goal_outcome_unknown{};
  /// The last goal was cancelled and the controller's reference still moves
  /// (JTC reports a cancelled goal finished before it brakes).
  bool decelerating{};
  /// The reset-position writer sequence (Mock, Isaac) is running.
  bool reset_in_progress{};
  /// How long the earliest-ending open goal (the Stop's braking goal
  /// included) is past its expected end without its result (negative before
  /// it), on the controller's clock; absent without an open goal.
  std::optional<std::int64_t> goal_past_end_ns;
  /// The lane's existing bound for a goal's result after its expected end
  /// (`hard_stop_completion_timeout_s`; the adapter sets it for an arm).
  std::optional<std::int64_t> goal_result_bound_ns;
  /// The component's own streams (measured joints, gripper feedback).
  std::vector<core::ComponentStream> streams;
  /// The component runs in a simulator (`isaac` lane): its streams stop when
  /// the simulation is not playing, which is simulator state, never a fault.
  bool simulated{};
};

/// The component state from one observation, in order:
/// - an exited launch is `fault` (`fault_process_exited`);
/// - a guard or tool-context fault is `fault`, cleared by Recover;
/// - a stale own stream is `fault` (`fault_stream_stale`), except in a
///   simulator;
/// - an unreachable controller manager is `offline`;
/// - the controller active on active hardware is `enabled`; loaded on active
///   hardware `prepared`; otherwise `not_prepared`;
/// - in a simulator, a stale own stream keeps that lifecycle and adds the
///   reason `simulation_not_playing` (never a fault, never a latch).
/// Activity: `busy` while a goal runs, the reference still decelerates after
/// a cancel or a reset runs; `unknown` when a goal is open but the
/// controller manager cannot report its result, or its result hasn't come
/// back by its expected end plus the lane's bound; else `idle`.
[[nodiscard]] core::ComponentState
ros2_control_component_state(const Ros2ControlObservation &observation,
                             std::int64_t now_ns);

} // namespace manyforge::roboplan_backend
