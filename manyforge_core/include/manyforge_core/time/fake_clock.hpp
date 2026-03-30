#pragma once

#include <manyforge_core/time/iclock.hpp>

namespace manyforge::core {

/// A fake clock for testing that starts at 0 and can be manually advanced.
/// Not thread-safe. Intended for deterministic unit tests.
class FakeClock final : public IClock {
 public:
  /// Constructs a FakeClock starting at time 0.
  FakeClock() : current_time_(0) {}

  /// Returns the current time.
  TimePoint now() const override { return current_time_; }

  /// Advances the clock by the given duration.
  void advance(Duration duration) { current_time_ += duration; }

  /// Sets the clock to an absolute time point.
  void set(TimePoint time_point) { current_time_ = time_point; }

 private:
  TimePoint current_time_;
};

}  // namespace manyforge::core
