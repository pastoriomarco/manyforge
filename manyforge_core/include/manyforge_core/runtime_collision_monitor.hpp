#pragma once

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/error_code.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace manyforge::core {

struct RuntimeCollisionMonitorTimingMetrics {
  std::uint64_t callbacks_started = 0;
  std::uint64_t scheduled_to_start_jitter_max_ns = 0;
  std::uint64_t callback_duration_max_ns = 0;
  std::uint64_t late_periods = 0;
  std::uint64_t missed_periods = 0;
  std::uint64_t max_consecutive_late_periods = 0;
  /// Runs of late checks (OD-217): a run starts at the first check that ends
  /// one or more periods after its deadline and ends at the next on-time one.
  std::uint64_t late_episodes = 0;
};

/// The first late check of an episode (OD-217), handed to the owner's hook.
struct RuntimeCollisionMonitorLateEpisode {
  /// How long after its deadline this check ended.
  std::uint64_t late_by_ns = 0;
  /// Whole periods this check overran (skipped, never caught up).
  std::uint64_t missed_periods = 0;
  std::uint64_t check_duration_ns = 0;
  /// Totals of this monitor after this check.
  RuntimeCollisionMonitorTimingMetrics totals;
};

/// Owns runtime-collision monitor scheduling, cancellation, and teardown
/// quiescence. Collision semantics remain with the generation-bound query
/// session; this collaborator only decides when ticks run and guarantees
/// callbacks cannot outlive stop().
class RuntimeCollisionMonitor {
 public:
  struct Config {
    Duration period_ns = 50'000'000;
  };

  using TickCallback = std::function<Result<void>()>;
  /// Called from the monitor's scheduled callback while the monitor mutex is
  /// held. Implementations must not re-enter RuntimeCollisionMonitor
  /// start/stop, and scheduler cancel/drain implementations must not wait on a
  /// callback that is blocked trying to reacquire the same monitor mutex.
  using ShouldContinueCallback = std::function<bool()>;
  /// Called on the check thread, outside the monitor mutex, once at the start
  /// of each late episode. It must not block or write to a stream.
  using LateEpisodeCallback =
      std::function<void(const RuntimeCollisionMonitorLateEpisode&)>;

  /// `scheduler`'s RuntimeSafety lane runs this monitor's checks. In the
  /// host it is the robot's own check thread (OD-217); fixtures may share one.
  RuntimeCollisionMonitor(IClock& clock,
                          IScheduler& scheduler,
                          Config config,
                          TickCallback tick_callback,
                          ShouldContinueCallback should_continue_callback,
                          LateEpisodeCallback late_episode_callback = {});
  ~RuntimeCollisionMonitor();

  RuntimeCollisionMonitor(const RuntimeCollisionMonitor&) = delete;
  RuntimeCollisionMonitor& operator=(const RuntimeCollisionMonitor&) = delete;
  RuntimeCollisionMonitor(RuntimeCollisionMonitor&&) = delete;
  RuntimeCollisionMonitor& operator=(RuntimeCollisionMonitor&&) = delete;

  /// What a stop() waits for after it has cancelled the pending tick.
  enum class StopWait {
    /// This monitor's own in-flight callbacks, bounded (250 ms).
    kOwnCallbacksBounded,
    /// This monitor's own in-flight callbacks, until they have returned.
    /// Does not wait for the RuntimeSafety lane, so it is not for detach or
    /// destruction: a returning callback still touches the monitor object
    /// after the in-flight count drops.
    kOwnCallbacks,
    /// The RuntimeSafety lane of this monitor's scheduler (in the host, the
    /// robot's own check thread), then this monitor's own in-flight
    /// callbacks, both until done (detach, destruction).
    kLaneAndOwnCallbacks,
  };

  [[nodiscard]] bool in_callback() const noexcept;
  void start();
  void stop(bool drain_callbacks = false);
  void stop(StopWait wait);
  // Caller first closes and drains external start/stop admission.
  [[nodiscard]] Result<void> detach();

  /// Install a best-effort evidence hook invoked once when a draining stop()
  /// misses its bound and starts waiting for the stuck drain (e.g. the
  /// kernel's flight-ring dump). Startup wiring only; the hook must not block.
  void set_fail_closed_dump_hook(std::function<void()> hook);

  [[nodiscard]] RuntimeCollisionMonitorTimingMetrics timing_metrics() const
      noexcept;

  /// OD-217 deadman stamp. The tick calls this after a check found the robot
  /// sample it used clear, with that sample's receive time on this clock;
  /// the newest time is kept. A failed or inconclusive check does not call it.
  void note_verified_sample(TimePoint received_ns) noexcept;
  /// The receive time of the newest sample verified clear since the last
  /// start(), or that start()'s time when none is newer. One atomic load.
  [[nodiscard]] TimePoint verified_sample_time_ns() const noexcept {
    return verified_sample_ns_.load(std::memory_order_relaxed);
  }
  /// OD-217 second stamp: the tick calls this when a check that found its
  /// sample clear has finished, with its finish time on this clock. It only
  /// chooses the deadman's reason (checks not running, or position feedback
  /// old); it never moves the deadman's time.
  void note_check_finished(TimePoint finished_ns) noexcept {
    check_finished_ns_.store(finished_ns, std::memory_order_relaxed);
  }
  /// The finish time of the newest clear check (0 before the first). One
  /// atomic load.
  [[nodiscard]] TimePoint check_finished_time_ns() const noexcept {
    return check_finished_ns_.load(std::memory_order_relaxed);
  }

 private:
  void schedule_next_locked(std::uint64_t generation, TimePoint deadline);

  IClock& clock_;
  IScheduler& scheduler_;
  // Set once at startup wiring, read only when a draining stop() is stuck.
  std::function<void()> fail_closed_dump_hook_;
  Config config_;
  TickCallback tick_callback_;
  ShouldContinueCallback should_continue_callback_;
  LateEpisodeCallback late_episode_callback_;

  mutable std::mutex mutex_;
  std::mutex detach_mutex_;
  std::condition_variable cv_;
  ScheduleHandle handle_{0};
  std::uint64_t generation_ = 0;
  bool active_ = false;
  bool detached_ = false;
  bool detach_completed_ = false;
  std::uint64_t callbacks_in_flight_ = 0;

  std::atomic<std::uint64_t> timing_callbacks_started_{0};
  std::atomic<std::uint64_t> timing_start_jitter_max_ns_{0};
  std::atomic<std::uint64_t> timing_callback_duration_max_ns_{0};
  std::atomic<std::uint64_t> timing_late_periods_{0};
  std::atomic<std::uint64_t> timing_missed_periods_{0};
  std::atomic<std::uint64_t> timing_consecutive_late_periods_{0};
  std::atomic<std::uint64_t> timing_max_consecutive_late_periods_{0};
  std::atomic<std::uint64_t> timing_late_episodes_{0};
  std::atomic<TimePoint> verified_sample_ns_{0};
  std::atomic<TimePoint> check_finished_ns_{0};
};

}  // namespace manyforge::core
