#pragma once

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/stop_terminal_state.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace manyforge::roboplan_backend {

// Cell-wide bounds belong to the native cell owner, not to any particular
// motion or gripper runtime. This is the ceiling an Apply may grow the cell
// to; the Kernel pre-starts Stop-stage attempt workers only for the current
// cell's Stop-qualified participants and resizes them at each Apply commit.
inline constexpr std::size_t kNativeMaximumLiveStopParticipants = 64U;

/// Linux scheduling request for one native scheduler role.
///
/// The default (no policy and no affinity) preserves inherited SCHED_OTHER
/// behavior. A strict deployment profile marks its controller/safety/stop
/// requests required so kernel construction fails before any callback can run
/// when the OS rejects the requested resource contract.
struct SchedulerWorkerOsConfig {
  std::optional<int> policy;
  int priority = 0;
  std::vector<int> cpu_affinity;
  bool required = false;
  /// OD-59 (strict profile): touch a bounded part of the worker's stack
  /// before it reports startup, so its pages are resident (and locked).
  bool prefault_stack = false;

  bool requested() const {
    return policy.has_value() || !cpu_affinity.empty();
  }
};

struct SimpleSchedulerConfig {
  std::array<SchedulerWorkerOsConfig, core::kSchedulerPriorityCount> workers;
  /// OD-59 (strict profile): before any worker starts, keep freed heap and
  /// avoid mmap allocations (`mallopt`), then lock the process memory
  /// (`mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)`). A failure refuses
  /// the scheduler's construction like a rejected worker profile.
  bool lock_memory = false;

  static SimpleSchedulerConfig from_environment();
  static SimpleSchedulerConfig from_environment(
      const std::map<std::string, std::string>& environment);
};

/// Wall clock using std::chrono::steady_clock.
class WallClock final : public core::IClock {
 public:
  core::TimePoint now() const override;
};

/// Production scheduler with role-split worker threads.
///
/// Each SchedulerPriority owns an independent single-thread worker. This keeps
/// controller-command progress, runtime collision ticks, stop-control deadlines,
/// lifecycle work, monitoring, and background callbacks from starving each
/// other while preserving deterministic per-role drain semantics.
///
/// Cancellation is serialised through a shared registry that records
/// the worker each handle belongs to.
class SimpleScheduler final : public core::IScheduler {
 public:
  SimpleScheduler();
  explicit SimpleScheduler(SimpleSchedulerConfig config);
  ~SimpleScheduler() override;

  core::ScheduleHandle schedule(
      core::TimePoint deadline, std::function<void()> callback,
      core::SchedulerPriority priority =
          core::SchedulerPriority::Lifecycle) override;
  // Removes a queued item without waiting for a dequeued callback to finish.
  void cancel(core::ScheduleHandle handle) override;
  bool cancel_before_dequeue(core::ScheduleHandle handle) override;
  void drain(core::SchedulerPriority priority) override;
  bool drain_for(core::SchedulerPriority priority,
                 std::chrono::nanoseconds timeout) override;
  void drain_all() override;
  bool drain_all_for(std::chrono::nanoseconds timeout) override;
  /// Close scheduling and discard work not yet dequeued. Call only after
  /// component owners have settled their tracked obligations.
  void close_admission_and_discard();

  /// The host installs its cell containment once its addressed Kernel exists
  /// and clears it (empty sink) before that Kernel is destroyed. Clearing
  /// waits for a containment already in progress.
  void set_safety_path_fault_sink(std::function<void(std::string_view)> sink);
  /// Hands the typed reason to the installed sink; terminates without one.
  void contain_safety_path_fault(std::string_view reason_code) noexcept override;

 private:
  struct ScheduledTask {
    core::TimePoint deadline = 0;
    std::function<void()> callback;
  };

  /// One worker pool. Each priority lane owns one of these.
  struct Worker {
    SimpleScheduler* owner = nullptr;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::condition_variable drain_cv;
    std::map<core::ScheduleHandle, ScheduledTask> tasks;
    std::jthread thread;
    std::size_t callbacks_in_flight = 0;
    std::uint64_t queue_generation = 0;
    /// Callback throws on this lane; only the first is logged. Touched only
    /// by this worker's thread.
    std::uint64_t callback_exceptions = 0;
    core::SchedulerPriority priority = core::SchedulerPriority::Lifecycle;
    const char* thread_name = nullptr;
    SchedulerWorkerOsConfig os_config;
    std::mutex startup_mutex;
    std::condition_variable startup_cv;
    bool startup_complete = false;
    std::string startup_error;

    void worker_loop(std::stop_token st);
    void ensure_started_locked(Worker* self);
    void drain();
    bool drain_for(std::chrono::nanoseconds timeout);
  };

  /// Pick the worker that should own a callback for the given priority.
  Worker& worker_for(core::SchedulerPriority priority);

  std::atomic<uint64_t> next_handle_{1};

  /// Maps handle -> owning worker so cancel() can route correctly
  /// without locking both workers.
  std::mutex registry_mutex_;
  bool admission_closed_ = false;
  std::map<core::ScheduleHandle, core::SchedulerPriority> handle_to_priority_;

  /// Held while the sink runs so clearing it cannot race a containment.
  /// Declared before the workers, which join first on destruction.
  std::mutex safety_path_fault_sink_mutex_;
  std::function<void(std::string_view)> safety_path_fault_sink_;

  std::array<Worker, core::kSchedulerPriorityCount> workers_;
};

}  // namespace manyforge::roboplan_backend
