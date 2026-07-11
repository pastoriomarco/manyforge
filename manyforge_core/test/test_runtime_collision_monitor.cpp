#include <manyforge_core/runtime_collision_monitor.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace manyforge::core {
namespace {

TEST(RuntimeCollisionMonitorTest, StartSchedulesTickAndReschedulesWhileActive) {
  FakeClock clock;
  ManualScheduler scheduler;
  int tick_count = 0;
  bool keep_running = true;

  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        ++tick_count;
        return Result<void>::Ok();
      },
      [&]() { return keep_running; }};

  monitor.start();
  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 1);

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 2);

  keep_running = false;
  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 3);

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 3);
}

TEST(RuntimeCollisionMonitorTest, StopCancelsPendingTick) {
  FakeClock clock;
  ManualScheduler scheduler;
  int tick_count = 0;

  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        ++tick_count;
        return Result<void>::Ok();
      },
      []() { return true; }};

  monitor.start();
  monitor.stop();

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 0);
}

TEST(RuntimeCollisionMonitorTest, RestartInvalidatesPreviouslyScheduledTick) {
  FakeClock clock;
  ManualScheduler scheduler;
  int tick_count = 0;

  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        ++tick_count;
        return Result<void>::Ok();
      },
      []() { return false; }};

  monitor.start();
  monitor.start();

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 1);
}

TEST(RuntimeCollisionMonitorTest, StopDrainWaitsForInFlightCallback) {
  FakeClock clock;
  ManualScheduler scheduler;
  std::promise<void> callback_entered;
  auto callback_entered_future = callback_entered.get_future();
  std::promise<void> release_callback;
  auto release_callback_future = release_callback.get_future();
  std::atomic<bool> stop_returned{false};

  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        callback_entered.set_value();
        release_callback_future.wait();
        return Result<void>::Ok();
      },
      []() { return true; }};

  monitor.start();
  clock.advance(10'000'000);
  std::thread scheduler_thread([&]() { scheduler.advance_to(clock.now()); });
  ASSERT_EQ(callback_entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  std::thread stop_thread([&]() {
    monitor.stop(/*drain_callbacks=*/true);
    stop_returned.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(stop_returned.load(std::memory_order_acquire));

  release_callback.set_value();
  stop_thread.join();
  scheduler_thread.join();
  EXPECT_TRUE(stop_returned.load(std::memory_order_acquire));
}

TEST(RuntimeCollisionMonitorTest, StopFromInsideCallbackDoesNotDeadlock) {
  FakeClock clock;
  ManualScheduler scheduler;
  RuntimeCollisionMonitor* monitor_ptr = nullptr;
  int tick_count = 0;

  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        ++tick_count;
        monitor_ptr->stop(/*drain_callbacks=*/true);
        return Result<void>::Ok();
      },
      []() { return true; }};
  monitor_ptr = &monitor;

  monitor.start();
  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 1);

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 1);
}

TEST(RuntimeCollisionMonitorTest,
     TimingMetricsExposeJitterDurationAndMissedPeriods) {
  FakeClock clock;
  ManualScheduler scheduler;
  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        clock.advance(3'000'000);
        return Result<void>::Ok();
      },
      []() { return false; }};

  monitor.start();
  clock.advance(35'000'000);
  scheduler.advance_to(clock.now());

  const auto metrics = monitor.timing_metrics();
  EXPECT_EQ(metrics.callbacks_started, 1u);
  EXPECT_EQ(metrics.scheduled_to_start_jitter_max_ns, 25'000'000u);
  EXPECT_EQ(metrics.callback_duration_max_ns, 3'000'000u);
  EXPECT_EQ(metrics.late_periods, 1u);
  EXPECT_EQ(metrics.missed_periods, 2u);
  EXPECT_EQ(metrics.max_consecutive_late_periods, 1u);
}

TEST(RuntimeCollisionMonitorTest,
     CallbackOverrunCountsMissedPeriodsWithoutSchedulerStartDelay) {
  FakeClock clock;
  ManualScheduler scheduler;
  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        clock.advance(25'000'000);
        return Result<void>::Ok();
      },
      []() { return false; }};

  monitor.start();
  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());

  const auto metrics = monitor.timing_metrics();
  EXPECT_EQ(metrics.callbacks_started, 1u);
  EXPECT_EQ(metrics.scheduled_to_start_jitter_max_ns, 0u);
  EXPECT_EQ(metrics.callback_duration_max_ns, 25'000'000u);
  EXPECT_EQ(metrics.late_periods, 1u);
  EXPECT_EQ(metrics.missed_periods, 2u);
  EXPECT_EQ(metrics.max_consecutive_late_periods, 1u);
}

}  // namespace
}  // namespace manyforge::core
