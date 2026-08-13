#pragma once

#include <chrono>
#include <cstddef>
#include <manyforge_core/ports/istop_control_executor.hpp>
#include <memory>

namespace manyforge::core {

/// Hard bound: 64 correlation slots plus one non-evictable Full-Stop slot.
inline constexpr std::size_t kMaximumStopControlWorkSlots = 65U;

/// One fixed-capacity StopControl worker. Construction performs every dynamic
/// allocation, starts the worker, and waits for its readiness publication.
/// Runtime ingress touches only atomics plus a condition-variable notification.
///
/// The receiver must outlive this object. Destruction closes ingress, requests
/// worker exit, and joins it, so no callback can survive executor teardown.
/// Normal runtime shutdown must first terminalize active Stops, cancel their
/// deadlines, CloseIngress(), and successfully DrainFor(); a failed bounded
/// drain keeps the enclosing runtime and receiver alive.
class BoundedStopControlExecutor final : public IStopControlExecutor {
 public:
  BoundedStopControlExecutor(std::size_t capacity, IClock& clock,
                             IStopControlWorkReceiver& receiver);
  ~BoundedStopControlExecutor() override;

  BoundedStopControlExecutor(const BoundedStopControlExecutor&) = delete;
  BoundedStopControlExecutor& operator=(const BoundedStopControlExecutor&) = delete;
  BoundedStopControlExecutor(BoundedStopControlExecutor&&) = delete;
  BoundedStopControlExecutor& operator=(BoundedStopControlExecutor&&) = delete;

  [[nodiscard]] std::size_t capacity() const noexcept override;
  [[nodiscard]] bool prequalified_ready() const noexcept override;
  void Publish(StopControlWorkToken token) noexcept override;
  void ArmDeadline(StopControlWorkToken token, TimePoint deadline) noexcept override;
  void CancelDeadline(StopControlWorkToken token) noexcept override;
  void CloseIngress() noexcept override;
  [[nodiscard]] bool DrainFor(std::chrono::nanoseconds timeout) noexcept override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge::core
