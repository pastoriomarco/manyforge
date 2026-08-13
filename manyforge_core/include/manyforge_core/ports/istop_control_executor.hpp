#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <manyforge_core/time/iclock.hpp>

namespace manyforge::core {

using StopControlSlotIndex = std::uint32_t;
inline constexpr StopControlSlotIndex kInvalidStopControlSlot =
    std::numeric_limits<StopControlSlotIndex>::max();

/// One physical preallocated StopControl work slot and its monotonic reuse
/// generation. The emergency Stop reserve mints this token; a correlation
/// slot uses [0, configured correlation capacity) and the non-evictable Full
/// Stop slot uses exactly configured correlation capacity.
struct StopControlWorkToken final {
  StopControlSlotIndex slot{kInvalidStopControlSlot};
  std::uint64_t generation{};

  [[nodiscard]] constexpr bool locally_valid() const noexcept {
    return slot != kInvalidStopControlSlot && generation != 0U;
  }

  friend constexpr bool operator==(const StopControlWorkToken&,
                                   const StopControlWorkToken&) = default;
};

/// Kernel-owned Stop transaction progress receiver. It must outlive the
/// executor and may be called repeatedly for one token: once for initial
/// handoff and again when an armed deadline becomes due.
class IStopControlWorkReceiver {
 public:
  virtual ~IStopControlWorkReceiver() = default;
  virtual void ProgressStop(StopControlWorkToken token) noexcept = 0;
};

/// Allocation-free ingress to one already-started StopControl worker.
///
/// Publish(), ArmDeadline(), and CancelDeadline() never wait for receiver work,
/// allocate, throw, or report ordinary queue pressure. A valid token maps to a
/// dedicated preallocated slot, so exact repeats coalesce instead of consuming
/// queue capacity. Invalid/out-of-capacity tokens are programming defects and
/// have no effect; motion admission must prove the reserve/executor capacity
/// join before exposing Stop ingress.
///
/// Publish may be called concurrently. ArmDeadline and CancelDeadline for one
/// physical slot are serialized by the Kernel Stop owner; calls for disjoint
/// slots may proceed concurrently. This preserves an exact generation/deadline
/// pair without adding a lock or retry loop to emergency ingress.
class IStopControlExecutor {
 public:
  virtual ~IStopControlExecutor() = default;

  [[nodiscard]] virtual std::size_t capacity() const noexcept = 0;
  [[nodiscard]] virtual bool prequalified_ready() const noexcept = 0;

  virtual void Publish(StopControlWorkToken token) noexcept = 0;
  virtual void ArmDeadline(StopControlWorkToken token, TimePoint deadline) noexcept = 0;
  virtual void CancelDeadline(StopControlWorkToken token) noexcept = 0;

  /// Reject future Publish/Arm calls. Existing work and deadlines remain live.
  virtual void CloseIngress() noexcept = 0;

  /// Wait for every accepted publication, deadline, and receiver callback to
  /// drain. This is a shutdown/lifetime join, not a Stop hot-path operation.
  [[nodiscard]] virtual bool DrainFor(std::chrono::nanoseconds timeout) noexcept = 0;
};

}  // namespace manyforge::core
