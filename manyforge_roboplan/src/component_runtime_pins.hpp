#pragma once

#include <memory>
#include <vector>

namespace manyforge::roboplan_backend {
struct RetainedExecutionHost;
class RetainedGripperRuntime;

namespace host_detail {
class IoProviderDriverRecord;
using IoProviderDriverGroup =
    std::vector<std::shared_ptr<IoProviderDriverRecord>>;

// A lifetime pin for the complete actual component, not another runtime owner.
// Physical-only components retain their established identity. When I/O joins
// a component, retention must preserve every member of this immutable group.
struct ComponentRuntimePins final {
  std::shared_ptr<RetainedExecutionHost> runtime;
  std::shared_ptr<RetainedGripperRuntime> gripper_runtime;
  std::shared_ptr<const IoProviderDriverGroup> io_provider_drivers;

  [[nodiscard]] bool
  matches(const std::shared_ptr<RetainedExecutionHost> &motion,
          const std::shared_ptr<RetainedGripperRuntime> &gripper,
          const std::shared_ptr<const IoProviderDriverGroup> &drivers)
      const noexcept {
    return runtime == motion && gripper_runtime == gripper &&
           io_provider_drivers == drivers;
  }
};
} // namespace host_detail
} // namespace manyforge::roboplan_backend
