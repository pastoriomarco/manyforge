// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <memory>

namespace manyforge::ros2_control_execution { class Ros2TransportResources; }

namespace manyforge::roboplan_backend {

/// Existing adapter's ROS entities, staged without replacing the adapter.
/// Host serializes the transaction and pins stages through every commit; it
/// activates the common gate only after publishing the replacement owner.
class AdapterTransportConsumer {
public:
  using Owner = ros2_control_execution::Ros2TransportResources;
  using Gate = std::shared_ptr<std::atomic<bool>>;
  virtual ~AdapterTransportConsumer() = default;
  virtual std::shared_ptr<void> Stage(const std::shared_ptr<Owner> &expected,
                                      const std::shared_ptr<Owner> &replacement,
                                      Gate gate) = 0;
  virtual bool Validate(const std::shared_ptr<void> &staged) const = 0;
  virtual void Commit(const std::shared_ptr<void> &staged) noexcept = 0;
};

} // namespace manyforge::roboplan_backend
