#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <stop_token>

namespace manyforge::core {

/// Owns the small cross-thread boundary between the runtime collision monitor
/// and a motion-trip controlled stop. The monitor may only signal the stop
/// worker through this coordinator; the worker remains the sole owner of the
/// terminal decision.
class MotionTripStopCoordinator {
 public:
  struct FinishResult {
    bool collision_detected = false;
  };

  MotionTripStopCoordinator() = default;
  ~MotionTripStopCoordinator() = default;

  MotionTripStopCoordinator(const MotionTripStopCoordinator&) = delete;
  MotionTripStopCoordinator& operator=(const MotionTripStopCoordinator&) =
      delete;
  MotionTripStopCoordinator(MotionTripStopCoordinator&&) = delete;
  MotionTripStopCoordinator& operator=(MotionTripStopCoordinator&&) = delete;

  void begin(std::stop_source stop_source);
  void deactivate();
  FinishResult finish();
  void reset();

  bool active() const;
  bool collision_detected() const;

  /// Requests cancellation of the in-flight controlled stop, if one still
  /// exists. Returns true when a stop source was available to signal.
  bool request_stop();

  /// Records a stop-path collision and cancels the in-flight controlled stop,
  /// if one still exists. Returns true when a stop source was available to
  /// signal.
  bool signal_collision_and_cancel();

 private:
  std::atomic<bool> active_{false};
  std::atomic<bool> collision_detected_{false};
  mutable std::mutex mutex_;
  std::optional<std::stop_source> stop_source_;
};

}  // namespace manyforge::core
