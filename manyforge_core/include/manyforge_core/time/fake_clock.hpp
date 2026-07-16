#pragma once

#include <atomic>
#include <manyforge_core/time/iclock.hpp>

namespace manyforge::core {

/// A fake clock for deterministic tests that starts at 0 and can be advanced.
/// Reads and mutations are thread-safe so a test may drive a deadline while a
/// worker reads the same injected time authority.
class FakeClock final : public IClock {
 public:
  /// Constructs a FakeClock starting at time 0.
  FakeClock() : current_time_(0) {}

  /// Returns the current time.
  TimePoint now() const override { return current_time_.load(std::memory_order_acquire); }

  /// Advances the clock by the given duration.
  void advance(Duration duration) { current_time_.fetch_add(duration, std::memory_order_acq_rel); }

  /// Sets the clock to an absolute time point.
  void set(TimePoint time_point) { current_time_.store(time_point, std::memory_order_release); }

 private:
  std::atomic<TimePoint> current_time_;
};

}  // namespace manyforge::core
