#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

using namespace std::chrono_literals;

class SnapshotExecutionBackend final : public IExecutionBackend {
 public:
  void SetMeasuredVelocities(std::vector<double> velocities) {
    std::lock_guard lock(mutex_);
    snapshot_.joint_positions_rad.assign(velocities.size(), 0.0);
    snapshot_.joint_velocities_rad_s = std::move(velocities);
    snapshot_.state_source = "measured_feedback";
  }

  Result<void> RetirePreparedExecution(std::uint64_t) override { return Result<void>::Ok(); }
  PrepareParticipantResultDTO Prepare(const PreparedOperationParticipantDTO& participant,
                                      std::uint64_t) override {
    return MakePrepareParticipantResult(
               participant, PrepareParticipantResultStateDTO::kRejected,
               ErrorDTO{std::string(kErrorInvalidRequest),
                        "snapshot-only test backend cannot prepare execution",
                        {{"reason", "test_backend_prepare_unavailable"}}})
        .value();
  }

  ExecutionHandle Dispatch(const PreparedOperationParticipantDTO&, const ControllerGoalId&,
                           ControllerGoalGeneration) override {
    return {};
  }

  ExecutionHandle execute_trajectory(const TrajectoryDTO&) override { return {}; }

  Result<void> sync_observed_state_before_planning() override { return Result<void>::Ok(); }

  StopResultDTO stop() override { return {true, std::nullopt}; }
  ResetResultDTO reset() override { return {true, std::nullopt}; }

  ExecutionSnapshot get_execution_snapshot() const override {
    std::lock_guard lock(mutex_);
    return snapshot_;
  }

  void retire_execution_progress_generation(std::uint64_t) noexcept override {}

  ExecutionProgressHazardClaimResult claim_execution_progress_hazard(
      std::uint64_t) noexcept override {
    return ExecutionProgressHazardClaimResult::kGenerationMismatch;
  }

  ParticipantStopStageResultDTO TranslateStopStage(const ResolvedParticipantStopStageDTO& stage,
                                                   const RemainingStopBudgetDTO&,
                                                   std::stop_token) override {
    return ParticipantStopStageResultDTO{stage,
                                         StopStageAttemptDispositionDTO::kNotDispatched,
                                         ParticipantStopStageOutcomeDTO::kFailed,
                                         std::nullopt,
                                         std::nullopt,
                                         std::nullopt,
                                         std::nullopt,
                                         std::nullopt,
                                         0,
                                         {},
                                         std::nullopt};
  }

 private:
  mutable std::mutex mutex_;
  ExecutionSnapshot snapshot_;
};

class WaitableManualScheduler final : public IScheduler {
 public:
  ScheduleHandle schedule(TimePoint deadline, std::function<void()> callback,
                          SchedulerPriority priority = SchedulerPriority::Lifecycle) override {
    const auto handle = scheduler_.schedule(deadline, std::move(callback), priority);
    {
      std::lock_guard lock(mutex_);
      ++schedule_count_;
    }
    scheduled_.notify_all();
    return handle;
  }

  void cancel(ScheduleHandle handle) override { scheduler_.cancel(handle); }

  bool WaitForScheduleCount(std::size_t expected) {
    std::unique_lock lock(mutex_);
    return scheduled_.wait_for(lock, 1s, [&]() { return schedule_count_ >= expected; });
  }

  void AdvanceTo(TimePoint deadline) { scheduler_.advance_to(deadline); }

  std::size_t PendingCount() const {
    return scheduler_.pending_count(SchedulerPriority::StopControl);
  }

  SchedulerPriority LastPriority() const { return scheduler_.last_priority(); }

 private:
  ManualScheduler scheduler_;
  std::mutex mutex_;
  std::condition_variable scheduled_;
  std::size_t schedule_count_ = 0;
};

class StopOnExit final {
 public:
  explicit StopOnExit(std::stop_source& source) : source_(source) {}
  ~StopOnExit() { source_.request_stop(); }

  StopOnExit(const StopOnExit&) = delete;
  StopOnExit& operator=(const StopOnExit&) = delete;

 private:
  std::stop_source& source_;
};

TEST(ExecutionStopSupportTest, MeasuredRestPollAdvancesOnlyThroughInjectedClockAndScheduler) {
  SnapshotExecutionBackend backend;
  backend.SetMeasuredVelocities({0.25, 0.0});
  FakeClock clock;
  WaitableManualScheduler scheduler;
  std::stop_source cancellation;

  auto result = std::async(std::launch::async, [&]() {
    return wait_for_measured_rest_after_confirmed_stop(backend, 0.01, 30'000'000, clock, scheduler,
                                                       cancellation.get_token());
  });
  StopOnExit stop_on_exit(cancellation);

  ASSERT_TRUE(scheduler.WaitForScheduleCount(1));
  EXPECT_EQ(scheduler.LastPriority(), SchedulerPriority::StopControl);
  EXPECT_EQ(result.wait_for(0ms), std::future_status::timeout);

  backend.SetMeasuredVelocities({0.0, 0.0});
  clock.advance(kExecutionPollNs);
  scheduler.AdvanceTo(clock.now());

  ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
  const auto settled = result.get();
  EXPECT_TRUE(settled.settle_signal_available);
  EXPECT_TRUE(settled.at_rest);
  EXPECT_FALSE(settled.timed_out);
  EXPECT_FALSE(settled.aborted);
  EXPECT_DOUBLE_EQ(settled.max_abs_velocity_rad_s.value_or(-1.0), 0.0);
  EXPECT_EQ(scheduler.PendingCount(), 0U);
}

TEST(ExecutionStopSupportTest, DeadlineIsEvaluatedAgainstInjectedClock) {
  SnapshotExecutionBackend backend;
  backend.SetMeasuredVelocities({0.25, 0.0});
  FakeClock clock;
  WaitableManualScheduler scheduler;
  std::stop_source cancellation;
  constexpr TimePoint deadline = 25'000'000;

  auto result = std::async(std::launch::async, [&]() {
    return wait_for_measured_rest_after_confirmed_stop(backend, 0.01, deadline, clock, scheduler,
                                                       cancellation.get_token());
  });
  StopOnExit stop_on_exit(cancellation);

  ASSERT_TRUE(scheduler.WaitForScheduleCount(1));
  EXPECT_EQ(result.wait_for(0ms), std::future_status::timeout);

  clock.set(deadline);
  scheduler.AdvanceTo(deadline);

  ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
  const auto timed_out = result.get();
  EXPECT_TRUE(timed_out.settle_signal_available);
  EXPECT_FALSE(timed_out.at_rest);
  EXPECT_TRUE(timed_out.timed_out);
  EXPECT_FALSE(timed_out.aborted);
  EXPECT_EQ(clock.now(), deadline);
  EXPECT_EQ(scheduler.PendingCount(), 0U);
}

TEST(ExecutionStopSupportTest, CancellationWakesAndCancelsThePendingSchedulerCallback) {
  SnapshotExecutionBackend backend;
  backend.SetMeasuredVelocities({0.25, 0.0});
  FakeClock clock;
  WaitableManualScheduler scheduler;
  std::stop_source cancellation;

  auto result = std::async(std::launch::async, [&]() {
    return wait_for_measured_rest_after_confirmed_stop(backend, 0.01, 30'000'000, clock, scheduler,
                                                       cancellation.get_token());
  });
  StopOnExit stop_on_exit(cancellation);

  ASSERT_TRUE(scheduler.WaitForScheduleCount(1));
  cancellation.request_stop();

  ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
  const auto cancelled = result.get();
  EXPECT_TRUE(cancelled.settle_signal_available);
  EXPECT_FALSE(cancelled.at_rest);
  EXPECT_TRUE(cancelled.timed_out);
  EXPECT_FALSE(cancelled.aborted);
  EXPECT_EQ(clock.now(), 0);
  EXPECT_EQ(scheduler.PendingCount(), 0U);
}

TEST(ExecutionStopSupportTest, AbortPredicateIsObservedAtTheNextInjectedPoll) {
  SnapshotExecutionBackend backend;
  backend.SetMeasuredVelocities({0.25, 0.0});
  FakeClock clock;
  WaitableManualScheduler scheduler;
  std::stop_source cancellation;
  std::atomic_bool abort_requested{false};

  auto result = std::async(std::launch::async, [&]() {
    return wait_for_measured_rest_after_confirmed_stop(
        backend, 0.01, 30'000'000, clock, scheduler, cancellation.get_token(),
        [&]() { return abort_requested.load(std::memory_order_acquire); });
  });
  StopOnExit stop_on_exit(cancellation);

  ASSERT_TRUE(scheduler.WaitForScheduleCount(1));
  abort_requested.store(true, std::memory_order_release);
  clock.advance(kExecutionPollNs);
  scheduler.AdvanceTo(clock.now());

  ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
  const auto aborted = result.get();
  EXPECT_FALSE(aborted.at_rest);
  EXPECT_TRUE(aborted.timed_out);
  EXPECT_TRUE(aborted.aborted);
  EXPECT_EQ(scheduler.PendingCount(), 0U);
}

}  // namespace
}  // namespace manyforge::core
