#pragma once

#include <vector>

namespace manyforge::roboplan_backend::host_detail {

/// The latest manual operations of the direct-control runs whose targets this
/// composition (or one of its participants) publishes. The kernel outlives an
/// Apply, so a run of a removed motion or gripper target is not this cell's
/// manual operation: the targets projection has no row for it.
template <typename CompositionT, typename KernelT, typename RunIdT>
[[nodiscard]] auto CurrentDirectControlManualOperations(
    const CompositionT &composition, const KernelT &kernel,
    const RunIdT &motion_run_id, const RunIdT &gripper_run_id) {
  bool motion_present = composition.direct_control_motion_binding.has_value();
  bool gripper_present = composition.direct_control_gripper_binding.has_value();
  for (const auto &participant : composition.participant_compositions) {
    motion_present =
        motion_present || participant->direct_control_motion_binding.has_value();
    gripper_present =
        gripper_present || participant->direct_control_gripper_binding.has_value();
  }
  std::vector<RunIdT> run_ids;
  if (motion_present) run_ids.push_back(motion_run_id);
  if (gripper_present) run_ids.push_back(gripper_run_id);
  return kernel.get_latest_run_operations(run_ids);
}

}  // namespace manyforge::roboplan_backend::host_detail
