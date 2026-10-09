#pragma once

#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Existing binding identities an execution occurrence was prepared under.
/// They keep fencing the key after Apply or provider replacement: a key whose
/// binding differs from the mechanism's current binding is stale, whatever its
/// goal identity. Backend `execution_generation` and ROS goal UUIDs are
/// adapter-private correlation and never substitute for these values.
struct ExecutionBindingIdentity final {
  ActiveControlConfigurationGeneration active_control_configuration_generation{0U};
  RuntimeEpoch provider_runtime_epoch{std::string{}};

  friend bool operator==(const ExecutionBindingIdentity&,
                         const ExecutionBindingIdentity&) = default;
};

/// Fixed-size projection of `ExecutionGoalKey` for real-time handoffs.
/// It contains only the key's counter fields and is not a separate identity:
/// the NRT side resolves the full key (operation, goal id, binding) before any
/// value crosses into the loop, and the loop compares only this projection
/// against the goal it currently executes. A zero field never matches.
struct ExecutionGoalFence final {
  PreparedExecutionId prepared_execution_id{0U};
  ControllerGoalGeneration controller_goal_generation{0U};

  [[nodiscard]] constexpr bool nonzero() const noexcept {
    return prepared_execution_id.nonzero() && controller_goal_generation.nonzero();
  }

  friend constexpr bool operator==(const ExecutionGoalFence&,
                                   const ExecutionGoalFence&) = default;
};

/// Identity of one execution occurrence. Minted by the Kernel at dispatch and
/// carried by dispatch, status, progress, Stop results and callbacks.
/// `controller_goal_generation` advances on every replacement of the
/// participant's goal (controlled-Stop replacement, spring-back), so stale
/// goal fencing is never vacuous. NRT value: it owns strings and must not be
/// copied inside the real-time loop; use `fence()` there.
struct ExecutionGoalKey final {
  OperationId operation_id{std::string{}};
  ControllerGoalId controller_goal_id{std::string{}};
  ControllerGoalGeneration controller_goal_generation{0U};
  PreparedExecutionId prepared_execution_id{0U};
  ExecutionBindingIdentity binding;

  [[nodiscard]] constexpr ExecutionGoalFence fence() const noexcept {
    return ExecutionGoalFence{prepared_execution_id, controller_goal_generation};
  }

  friend bool operator==(const ExecutionGoalKey&, const ExecutionGoalKey&) = default;
};

}  // namespace manyforge::core
