#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <manyforge_core/types/component_lifecycle.hpp>

namespace manyforge::roboplan_backend::detail::ros2_control_lifecycle_plans_internal {

[[nodiscard]] inline core::LifecycleFactValueDTO boolean_value(const bool value) {
  return {core::LifecycleFactValueKindDTO::kBoolean, value, std::nullopt,
          std::nullopt};
}

[[nodiscard]] inline core::LifecyclePredicateDTO
predicate(std::vector<core::ComponentLifecycleStateDTO> states,
          const std::vector<std::pair<const char *, bool>> &facts) {
  std::sort(states.begin(), states.end());
  std::vector<core::LifecycleFactPredicateDTO> values;
  for (const auto &[id, value] : facts) {
    values.push_back({core::LifecycleFactId(id), boolean_value(value)});
  }
  std::sort(values.begin(), values.end(), [](const auto &left, const auto &right) {
    return left.lifecycle_fact_id < right.lifecycle_fact_id;
  });
  return {std::move(states), std::move(values)};
}


[[nodiscard]] inline std::vector<core::LifecycleIntentPlanDTO>
retained_ros2_control_lifecycle_plans() {
  using State = core::ComponentLifecycleStateDTO;
  using Intent = core::ComponentLifecycleIntentKindDTO;
  constexpr const char *kReady = "external.controller_configuration_ready";
  constexpr const char *kLoaded = "external.controller_loaded";
  constexpr const char *kFault = "external.fault_present";
  constexpr const char *kTransport = "external.transport_ready";
  // Prepared (or already active): the trajectory controller is loaded on
  // active hardware and the controller manager is reachable.
  const auto prepared = predicate({State::kPrepared, State::kActive},
                                  {{kLoaded, true}, {kFault, false}, {kTransport, true}});
  const auto active = predicate(
      {State::kActive},
      {{kReady, true}, {kLoaded, true}, {kFault, false}, {kTransport, true}});
  const auto disabled = predicate(
      {State::kDisabled}, {{kReady, false}, {kFault, false}, {kTransport, true}});
  constexpr std::int64_t timeout = 30'000'000'000LL;
  // Design R6 / §7.5: the spawner loads the controller inactive; CCLC Enable
  // activates it and Disable deactivates it (`switch_controller`). Recover
  // (the operator's Reset of an incident, Q41) re-prepares the component;
  // the target driver's next Enable activates the controller again.
  return {
      {Intent::kPrepare, State::kPrepared, std::nullopt, prepared, timeout,
       {{core::LifecyclePrimitiveId("ros2_control.controller.prepare"),
         predicate({State::kDeclared, State::kDisabled},
                   {{kLoaded, true}, {kFault, false}, {kTransport, true}}),
         prepared}}},
      {Intent::kEnable, State::kActive, std::nullopt, active, timeout,
       {{core::LifecyclePrimitiveId("ros2_control.controller.activate"),
         predicate({State::kPrepared}, {{kReady, false},
                                        {kLoaded, true},
                                        {kFault, false},
                                        {kTransport, true}}),
         active}}},
      {Intent::kDisable, State::kDisabled, std::nullopt, disabled, timeout,
       {{core::LifecyclePrimitiveId("ros2_control.controller.deactivate"),
         predicate({State::kPrepared, State::kActive, State::kDegraded},
                   {{kTransport, true}}),
         disabled}}},
      {Intent::kRecover, State::kPrepared,
       predicate({State::kPrepared, State::kActive, State::kDegraded,
                  State::kFaulted},
                 {{kTransport, true}}),
       prepared, timeout,
       {{core::LifecyclePrimitiveId("ros2_control.controller.recover"),
         predicate({State::kDegraded, State::kFaulted}, {{kTransport, true}}),
         prepared}}},
  };
}

} // namespace manyforge::roboplan_backend::detail::ros2_control_lifecycle_plans_internal
