#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <vector>

#include <manyforge_core/ports/icomponent_adapter_port.hpp>

namespace manyforge::roboplan_backend {

/// CONTRACTS §6 "Components without a lane": the execution host's small
/// generic adapter of a component with no lane and no device of its own (a
/// passive tool, an I/O-driven gripper). `prepare`, `enable`, `disable` and
/// `recover` succeed at once; it has no streams and no capabilities. Its
/// activity follows its own actions: `busy` while the given source reports
/// one running (e.g. a pneumatic open/close), otherwise `idle`. ROS-free.
class PassiveComponentAdapter final : public core::IComponentAdapter {
public:
  explicit PassiveComponentAdapter(std::function<bool()> action_running = {});

  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override;
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token stop) override;
  [[nodiscard]] std::vector<core::Capability> capabilities() const override;

private:
  const std::function<bool()> action_running_;
  mutable std::mutex mutex_;
  core::ComponentLifecycle lifecycle_{core::ComponentLifecycle::kNotPrepared};
};

} // namespace manyforge::roboplan_backend
