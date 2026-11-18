#pragma once

#include <vector>

namespace manyforge::roboplan_backend::host_detail {

/// The latest manual result on every motion and gripper target this
/// composition (or one of its participants) publishes, looked up per
/// (component, command target): in a multi-robot cell each arm and each
/// gripper keeps its own. The kernel outlives an Apply, so a removed motion
/// or gripper target is not asked for: the targets projection has no row
/// for it.
template <typename TargetT, typename CompositionT, typename KernelT>
[[nodiscard]] auto CurrentDirectControlManualOperations(
    const CompositionT &composition, const KernelT &kernel) {
  std::vector<TargetT> targets;
  const auto add = [&](const auto &binding) {
    if (!binding.has_value()) return;
    const auto &value = binding->value();
    targets.push_back(TargetT{value.controllable_component_id, value.command_target_id});
  };
  const auto add_composition = [&](const auto &candidate) {
    add(candidate.direct_control_motion_binding);
    add(candidate.direct_control_gripper_binding);
  };
  add_composition(composition);
  for (const auto &participant : composition.participant_compositions)
    add_composition(*participant);
  return kernel.get_latest_manual_operations(targets);
}

}  // namespace manyforge::roboplan_backend::host_detail
