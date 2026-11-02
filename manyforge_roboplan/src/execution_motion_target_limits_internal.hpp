#pragma once

#include <manyforge_core/types/cell_graph.hpp>

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

using ExecutionMotionTargetKey =
    std::pair<core::ControllableComponentId, core::CommandTargetId>;

// ROS configuration covers the whole provider runtime unit. Native
// configuration has already been sliced to the selected component.
[[nodiscard]] inline bool execution_motion_target_limit_keys_match(
    std::span<const core::ControlBindingDTO> motion_bindings,
    std::span<const ExecutionMotionTargetKey> configured_targets,
    const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &selected_component_id,
    bool whole_runtime_unit) {
  std::vector<ExecutionMotionTargetKey> expected_targets;
  expected_targets.reserve(motion_bindings.size());
  for (const auto &binding : motion_bindings) {
    if (whole_runtime_unit
            ? binding.provider_runtime_unit_id != runtime_unit_id
            : binding.controllable_component_id != selected_component_id) {
      continue;
    }
    expected_targets.emplace_back(binding.controllable_component_id,
                                  binding.command_target_id);
  }
  std::sort(expected_targets.begin(), expected_targets.end());
  return std::equal(configured_targets.begin(), configured_targets.end(),
                    expected_targets.begin(), expected_targets.end());
}

} // namespace manyforge::roboplan_backend
