#pragma once

#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/iclock.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::ros2_control_execution::detail {

using SceneProjectionSlotId = std::size_t;

struct SceneProjectionToken {
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  std::uint64_t request_id = 0;
};

enum class SceneProjectionOutcome {
  kApplied,
  kMissed,
  kRetryExhausted,
  kSuperseded,
};

struct SceneProjectionCompletion {
  SceneProjectionToken token;
  SceneProjectionOutcome outcome = SceneProjectionOutcome::kMissed;
  core::ExecutionStateSinkStatus sink_status =
      core::ExecutionStateSinkStatus::kException;
  std::size_t attempts = 0;
};

using SceneProjectionCompletionCallback =
    std::function<void(const SceneProjectionCompletion&)>;

struct SceneProjectionWorkerMetrics {
  std::uint64_t misses_total = 0;
  std::uint64_t successful_updates_total = 0;
  std::uint64_t lag_max_ns = 0;
  std::int64_t last_success_timestamp_ns = 0;
  std::uint64_t terminal_retries_scheduled_total = 0;
  std::uint64_t terminal_retry_successes_total = 0;
  std::uint64_t terminal_retry_exhausted_total = 0;
  std::uint64_t active_requests_total = 0;
  std::uint64_t joint_requests_total = 0;
  std::uint64_t requests_coalesced_total = 0;
  std::uint64_t requests_discarded_total = 0;
  std::uint64_t requests_rejected_while_suspended_total = 0;
  std::uint64_t completion_callback_exceptions_total = 0;
  std::uint64_t apply_duration_max_ns = 0;
  std::uint64_t queue_lag_max_ns = 0;
};

/// Serializes non-authoritative scene projection away from controller progress.
///
/// Each registered target owns a capacity-one pending slot, so arm and gripper
/// state cannot overwrite each other while superseded states for the same target
/// coalesce. Terminal requests receive bounded retries while they remain newest.
/// Blocking setup/reset writers suspend and drain this worker before mutating the
/// same sink. The worker is deliberately SCHED_OTHER/NRT and never participates
/// in command or runtime-safety timing.
class SceneProjectionWorker final {
 public:
  class Suspension final {
   public:
    Suspension() = default;
    Suspension(const Suspension&) = delete;
    Suspension& operator=(const Suspension&) = delete;
    Suspension(Suspension&& other) noexcept;
    Suspension& operator=(Suspension&& other) noexcept;
    ~Suspension();

   private:
    friend class SceneProjectionWorker;
    explicit Suspension(SceneProjectionWorker* owner) : owner_(owner) {}
    void release() noexcept;

    SceneProjectionWorker* owner_ = nullptr;
  };

  SceneProjectionWorker(
      std::shared_ptr<core::IExecutionStateSink> sink,
      std::string arm_group_id,
      core::IClock& clock);
  ~SceneProjectionWorker();

  SceneProjectionWorker(const SceneProjectionWorker&) = delete;
  SceneProjectionWorker& operator=(const SceneProjectionWorker&) = delete;

  [[nodiscard]] SceneProjectionSlotId register_joint_slot(
      std::string group_id,
      std::string joint_name,
      SceneProjectionCompletionCallback completion_callback);

  [[nodiscard]] bool submit_active(std::vector<double> positions_rad,
                                   std::int64_t source_timestamp_ns);
  [[nodiscard]] bool submit_terminal(std::vector<double> positions_rad,
                                     std::int64_t source_timestamp_ns);
  [[nodiscard]] bool submit_active_joint(
      SceneProjectionSlotId slot_id,
      double position_rad,
      std::int64_t source_timestamp_ns,
      SceneProjectionToken token);
  [[nodiscard]] bool submit_terminal_joint(
      SceneProjectionSlotId slot_id,
      double position_rad,
      std::int64_t source_timestamp_ns,
      SceneProjectionToken token);

  /// Prevents new work from being admitted and discards queued work. The caller
  /// must then drain before performing an authoritative blocking sink write.
  [[nodiscard]] Suspension suspend();
  [[nodiscard]] SceneProjectionWorkerMetrics metrics() const;
  [[nodiscard]] bool drain_for(std::chrono::nanoseconds timeout);
  [[nodiscard]] bool stop_and_join_for(std::chrono::nanoseconds timeout) noexcept;

 private:
  enum class RequestKind { kActive, kTerminal };
  enum class TargetKind { kGroup, kJoint };

  struct Request {
    RequestKind kind = RequestKind::kActive;
    std::uint64_t sequence = 0;
    std::vector<double> positions_rad;
    double position_rad = 0.0;
    std::int64_t source_timestamp_ns = 0;
    SceneProjectionToken token;
    std::chrono::steady_clock::time_point submitted_at;
  };

  struct Slot {
    TargetKind target_kind = TargetKind::kGroup;
    std::string group_id;
    std::string joint_name;
    SceneProjectionCompletionCallback completion_callback;
    std::optional<Request> pending;
  };

  struct WorkItem {
    SceneProjectionSlotId slot_id = 0;
    TargetKind target_kind = TargetKind::kGroup;
    std::string group_id;
    std::string joint_name;
    SceneProjectionCompletionCallback completion_callback;
    Request request;
  };

  [[nodiscard]] bool submit_group(RequestKind kind,
                                  std::vector<double> positions_rad,
                                  std::int64_t source_timestamp_ns);
  [[nodiscard]] bool submit_joint(RequestKind kind,
                                  SceneProjectionSlotId slot_id,
                                  double position_rad,
                                  std::int64_t source_timestamp_ns,
                                  SceneProjectionToken token);
  [[nodiscard]] bool submit_locked(SceneProjectionSlotId slot_id,
                                   Request request);
  [[nodiscard]] bool has_pending_locked() const;
  [[nodiscard]] WorkItem take_next_locked();
  [[nodiscard]] bool request_superseded_locked(
      SceneProjectionSlotId slot_id,
      std::uint64_t sequence) const;
  void run(std::stop_token stop_token) noexcept;
  [[nodiscard]] bool wait_before_terminal_retry(
      std::stop_token stop_token,
      SceneProjectionSlotId slot_id,
      std::uint64_t request_sequence);
  void record_attempt(const Request& request,
                      core::ExecutionStateSinkResult result,
                      std::uint64_t apply_duration_ns,
                      std::uint64_t queue_lag_ns,
                      std::size_t attempt);
  void record_retry_exhausted();
  void invoke_completion(
      const SceneProjectionCompletionCallback& callback,
      const SceneProjectionCompletion& completion) noexcept;
  void resume_one_suspension() noexcept;

  std::shared_ptr<core::IExecutionStateSink> sink_;
  core::IClock& clock_;

  mutable std::mutex mutex_;
  std::condition_variable_any work_cv_;
  std::condition_variable drain_cv_;
  std::vector<Slot> slots_;
  bool in_flight_ = false;
  bool stopping_ = false;
  bool exited_ = false;
  bool joining_ = false;
  bool joined_ = false;
  std::size_t suspension_count_ = 0;
  std::uint64_t next_sequence_ = 1;
  SceneProjectionWorkerMetrics metrics_;
  std::mutex stop_mutex_;
  std::jthread worker_;
};

}  // namespace manyforge::ros2_control_execution::detail
