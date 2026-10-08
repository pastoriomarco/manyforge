#pragma once

#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/error_code.hpp>

#include <functional>
#include <mutex>

namespace manyforge::core {

enum class StopType { kSoft, kHard };

/// Policy layer for stop/reset operations.
class StopPolicy {
 public:
  using CacheInvalidationCallback = std::function<void()>;
  using StopCallback = std::function<Result<void>()>;
  using BeforeTerminalStopCallback = std::function<void()>;
  using TerminalTransitionCallback =
      std::function<Result<void>(ExecutionState)>;
  using StopRequestedChangedCallback = std::function<void(bool)>;

  StopPolicy(ExecutionStateMachine& sm, IScheduler& scheduler, IClock& clock,
             CacheInvalidationCallback on_invalidate = nullptr,
             TerminalTransitionCallback terminal_transition = nullptr,
             StopRequestedChangedCallback on_stop_requested_changed = nullptr);
  ~StopPolicy();

  /// Soft stop with deceleration.
  /// From Executing: Executing → Stopping, schedules deferred Stopped.
  /// From Planning/Validating: direct → Stopped.
  Result<void> request_soft_stop(Duration deceleration_ns);

  /// Hard stop (immediate halt). Cancels pending soft stop.
  /// From Executing: → Stopping → Stopped.
  /// From Stopping: upgrades soft → hard (immediate Stopped).
  /// From Planning/Validating: direct → Stopped.
  Result<void> request_hard_stop(
      BeforeTerminalStopCallback before_terminal = nullptr);

  /// Reset to Idle. Valid from any non-Idle state.
  /// From terminal states: direct → Idle.
  /// From active states: stops first, then → Idle.
  Result<void> request_reset();

  void set_stop_callback(StopCallback callback);
  void cancel_pending_callbacks();
  bool stop_requested() const;

 private:
  ExecutionStateMachine& sm_;
  IScheduler& scheduler_;
  IClock& clock_;
  CacheInvalidationCallback on_invalidate_;
  TerminalTransitionCallback terminal_transition_;
  StopCallback stop_callback_;
  StopRequestedChangedCallback on_stop_requested_changed_;
  mutable std::mutex mutex_;
  bool stop_requested_{false};
  ScheduleHandle pending_soft_stop_{0};

  Result<void> transition_terminal(ExecutionState state);
  Result<void> invoke_stop_callback();
  Result<void> transition_intervention_after_stop_failure(
      const Result<void>& stop_result);
  void update_stop_requested(bool requested);
};

}  // namespace manyforge::core
