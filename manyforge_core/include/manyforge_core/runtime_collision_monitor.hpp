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

  RuntimeCollisionMonitor(IClock& clock,
                          IScheduler& scheduler,
                          Config config,
                          TickCallback tick_callback,
                          ShouldContinueCallback should_continue_callback);
  ~RuntimeCollisionMonitor();

  RuntimeCollisionMonitor(const RuntimeCollisionMonitor&) = delete;
  RuntimeCollisionMonitor& operator=(const RuntimeCollisionMonitor&) = delete;
  RuntimeCollisionMonitor(RuntimeCollisionMonitor&&) = delete;
  RuntimeCollisionMonitor& operator=(RuntimeCollisionMonitor&&) = delete;

  void start();
  void stop(bool drain_callbacks = false);

  /// Install a best-effort evidence hook invoked immediately before the
  /// monitor's fail-closed drain-timeout termination (e.g. the kernel's
  /// flight-ring dump). Startup wiring only; the hook must be noexcept in
  /// spirit — it runs on a dying process and must not block.
  void set_fail_closed_dump_hook(std::function<void()> hook);

  void set_active_contact_policy_id(std::string contact_policy_id);
  void clear_active_contact_policy_id();
  std::string active_contact_policy_id() const;
  [[nodiscard]] RuntimeCollisionMonitorTimingMetrics timing_metrics() const
      noexcept;

 private:
  void schedule_next_locked(std::uint64_t generation, TimePoint deadline);

  IClock& clock_;
  IScheduler& scheduler_;
  // Set once at startup wiring, read only on the fail-closed terminate path.
  std::function<void()> fail_closed_dump_hook_;
  Config config_;
  TickCallback tick_callback_;
  ShouldContinueCallback should_continue_callback_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  ScheduleHandle handle_{0};
  std::uint64_t generation_ = 0;
  bool active_ = false;
  std::uint64_t callbacks_in_flight_ = 0;

  mutable std::mutex contact_policy_mutex_;
  std::string active_contact_policy_id_;
  std::atomic<std::uint64_t> timing_callbacks_started_{0};
  std::atomic<std::uint64_t> timing_start_jitter_max_ns_{0};
  std::atomic<std::uint64_t> timing_callback_duration_max_ns_{0};
  std::atomic<std::uint64_t> timing_late_periods_{0};
  std::atomic<std::uint64_t> timing_missed_periods_{0};
  std::atomic<std::uint64_t> timing_consecutive_late_periods_{0};
  std::atomic<std::uint64_t> timing_max_consecutive_late_periods_{0};
};

}  // namespace manyforge::core
