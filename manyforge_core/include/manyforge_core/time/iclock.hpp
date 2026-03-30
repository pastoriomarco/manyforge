#pragma once

#include <chrono>
#include <cstdint>

namespace manyforge::core {

/// Time point as nanoseconds since epoch.
using TimePoint = int64_t;

/// Duration in nanoseconds.
using Duration = int64_t;

/// Abstract clock interface for injectable time.
/// ADR-0004: all time-dependent logic uses IClock, not wall time.
/// Production: backed by steady_clock or ROS time.
/// Tests: use FakeClock for deterministic behavior.
class IClock {
 public:
  virtual ~IClock() = default;

  /// Returns the current time in nanoseconds.
  virtual TimePoint now() const = 0;
};

}  // namespace manyforge::core
