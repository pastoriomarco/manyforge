#include <manyforge_core/runtime_collision_monitor.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::core {
namespace {

TEST(RuntimeCollisionMonitorTest, DetachedMonitorMayOutliveScheduler) {
  FakeClock clock;
  auto scheduler = std::make_unique<ManualScheduler>();
  auto monitor = std::make_unique<RuntimeCollisionMonitor>(
      clock, *scheduler, RuntimeCollisionMonitor::Config{10'000'000},
      [] { return Result<void>::Ok(); }, [] { return true; });
  monitor->start();
  ASSERT_TRUE(monitor->detach().ok());
  scheduler.reset();
  monitor->start();
  monitor->stop(true);
  ASSERT_TRUE(monitor->detach().ok());
  monitor.reset();
}

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

TEST(RuntimeCollisionMonitorTest, DetachFromOwnCallbackRefusesBeforeClosing) {
  FakeClock clock;
  ManualScheduler scheduler;
  RuntimeCollisionMonitor* monitor_ptr = nullptr;
  bool refused = false;
  RuntimeCollisionMonitor monitor{
      clock, scheduler, RuntimeCollisionMonitor::Config{10'000'000},
      [&] {
        const auto result = monitor_ptr->detach();
        refused = !result.ok() && result.error().details.at("reason") == "scheduler_not_ready";
        return Result<void>::Ok();
      }, [] { return true; }};
  monitor_ptr = &monitor;
  monitor.start();
  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_TRUE(refused);
  EXPECT_TRUE(monitor.detach().ok());
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

TEST(RuntimeCollisionMonitorTest, StuckDrainWaitsWithTypedLineInsteadOfTerminating) {
  // Lane drain misses its bound once, then completes: stop() reports the
  // stuck drain, runs the evidence hook once and keeps waiting.
  class FirstDrainStuckScheduler final : public IScheduler {
   public:
    ScheduleHandle schedule(TimePoint deadline, std::function<void()> callback,
                            SchedulerPriority priority) override {
      return scheduler_.schedule(deadline, std::move(callback), priority);
    }
    void cancel(ScheduleHandle handle) override { scheduler_.cancel(handle); }
    bool drain_for(SchedulerPriority, std::chrono::nanoseconds) override {
      return ++drain_calls > 1;
    }
    int drain_calls = 0;

   private:
    ManualScheduler scheduler_;
  };
  FakeClock clock;
  FirstDrainStuckScheduler scheduler;
  int dumps = 0;
  RuntimeCollisionMonitor monitor{
      clock, scheduler, RuntimeCollisionMonitor::Config{10'000'000},
      []() { return Result<void>::Ok(); }, []() { return true; }};
  monitor.set_fail_closed_dump_hook([&] { ++dumps; });
  monitor.start();

  testing::internal::CaptureStderr();
  monitor.stop(/*drain_callbacks=*/true);
  const auto output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(scheduler.drain_calls, 2);
  EXPECT_EQ(dumps, 1);
  EXPECT_NE(output.find("manyforge_fault code=drain_stuck "
                        "drain=runtime_collision_monitor.scheduler_runtime_safety_lane "
                        "bound_ms=1000 state=waiting_for_operator"),
            std::string::npos)
      << output;
}

// Models the shared RuntimeSafety lane: a lane drain waits until every
// callback already running on it has returned, like the production scheduler.
class LaneScheduler final : public IScheduler {
 public:
  ScheduleHandle schedule(TimePoint deadline, std::function<void()> callback,
                          SchedulerPriority priority) override {
    return inner.schedule(
        deadline,
        [this, callback = std::move(callback)]() {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            ++running_;
          }
          callback();
          {
            std::lock_guard<std::mutex> lock(mutex_);
            --running_;
          }
          cv_.notify_all();
        },
        priority);
  }
  void cancel(ScheduleHandle handle) override { inner.cancel(handle); }
  bool drain_for(SchedulerPriority priority,
                 std::chrono::nanoseconds bound) override {
    if (priority != SchedulerPriority::RuntimeSafety) return true;
    lane_drains.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, bound, [this] { return running_ == 0; });
  }

  ManualScheduler inner;
  std::atomic<int> lane_drains{0};

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  int running_ = 0;
};

// Another robot's monitor whose tick is held on the shared lane (a scheduler
// thread blocked inside it) until release().
class HeldLaneWork {
 public:
  HeldLaneWork(FakeClock& clock, LaneScheduler& scheduler)
      : clock_(clock),
        scheduler_(scheduler),
        other_(clock, scheduler, RuntimeCollisionMonitor::Config{10'000'000},
               [this]() {
                 entered_.set_value();
                 release_future_.wait();
                 return Result<void>::Ok();
               },
               []() { return false; }) {}
  ~HeldLaneWork() {
    release();
    if (thread_.joinable()) thread_.join();
  }

  [[nodiscard]] bool hold() {
    other_.start();
    clock_.advance(10'000'000);
    thread_ = std::thread([this]() { scheduler_.inner.advance_to(clock_.now()); });
    return entered_future_.wait_for(std::chrono::seconds(1)) ==
           std::future_status::ready;
  }
  void release() {
    if (released_) return;
    released_ = true;
    release_.set_value();
  }

 private:
  FakeClock& clock_;
  LaneScheduler& scheduler_;
  std::promise<void> entered_;
  std::future<void> entered_future_{entered_.get_future()};
  std::promise<void> release_;
  std::shared_future<void> release_future_{release_.get_future().share()};
  bool released_ = false;
  RuntimeCollisionMonitor other_;
  std::thread thread_;
};

// OD-190: a motion's start stops the previous monitor without waiting for
// other work on the shared lane.
TEST(RuntimeCollisionMonitorTest, OwnCallbacksStopDoesNotWaitForOtherLaneWork) {
  FakeClock clock;
  LaneScheduler scheduler;
  HeldLaneWork held{clock, scheduler};
  ASSERT_TRUE(held.hold());

  int tick_count = 0;
  RuntimeCollisionMonitor monitor{
      clock, scheduler, RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        ++tick_count;
        return Result<void>::Ok();
      },
      []() { return true; }};
  monitor.start();

  auto stopped = std::async(std::launch::async, [&]() {
    monitor.stop(RuntimeCollisionMonitor::StopWait::kOwnCallbacks);
  });
  const auto status = stopped.wait_for(std::chrono::milliseconds(500));
  held.release();
  stopped.wait();
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_EQ(scheduler.lane_drains.load(), 0);

  clock.advance(10'000'000);
  scheduler.inner.advance_to(clock.now());
  EXPECT_EQ(tick_count, 0);
}

TEST(RuntimeCollisionMonitorTest, OwnCallbacksStopStillWaitsForOwnInFlightCallback) {
  FakeClock clock;
  LaneScheduler scheduler;
  std::promise<void> callback_entered;
  auto callback_entered_future = callback_entered.get_future();
  std::promise<void> release_callback;
  auto release_callback_future = release_callback.get_future();
  std::atomic<bool> stop_returned{false};

  RuntimeCollisionMonitor monitor{
      clock, scheduler, RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        callback_entered.set_value();
        release_callback_future.wait();
        return Result<void>::Ok();
      },
      []() { return true; }};

  monitor.start();
  clock.advance(10'000'000);
  std::thread scheduler_thread([&]() { scheduler.inner.advance_to(clock.now()); });
  ASSERT_EQ(callback_entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  std::thread stop_thread([&]() {
    monitor.stop(RuntimeCollisionMonitor::StopWait::kOwnCallbacks);
    stop_returned.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(stop_returned.load(std::memory_order_acquire));

  release_callback.set_value();
  stop_thread.join();
  scheduler_thread.join();
  EXPECT_TRUE(stop_returned.load(std::memory_order_acquire));
  EXPECT_EQ(scheduler.lane_drains.load(), 0);
}

TEST(RuntimeCollisionMonitorTest, StaleOldGenerationCallbackDoesNotReachNewSession) {
  FakeClock clock;
  ManualScheduler scheduler;
  int session = 1;
  std::vector<int> sessions_ticked;
  RuntimeCollisionMonitor monitor{
      clock, scheduler, RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        sessions_ticked.push_back(session);
        return Result<void>::Ok();
      },
      []() { return false; }};
  monitor.start();  // old generation, due at 10 ms

  // Other lane work due earlier runs first, after the old tick was already
  // dequeued: it stops the monitor (own callbacks only), switches the
  // session and starts the new generation, as a motion's start does.
  (void)scheduler.schedule(
      5'000'000,
      [&]() {
        monitor.stop(RuntimeCollisionMonitor::StopWait::kOwnCallbacks);
        session = 2;
        monitor.start();
      },
      SchedulerPriority::RuntimeSafety);

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_TRUE(sessions_ticked.empty());

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(sessions_ticked, std::vector<int>{2});
}

TEST(RuntimeCollisionMonitorTest, DetachStillWaitsForTheWholeSharedLane) {
  FakeClock clock;
  LaneScheduler scheduler;
  HeldLaneWork held{clock, scheduler};
  ASSERT_TRUE(held.hold());
  RuntimeCollisionMonitor monitor{
      clock, scheduler, RuntimeCollisionMonitor::Config{10'000'000},
      []() { return Result<void>::Ok(); }, []() { return true; }};
  monitor.start();

  auto detached = std::async(std::launch::async, [&]() { return monitor.detach(); });
  EXPECT_EQ(detached.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  held.release();
  EXPECT_TRUE(detached.get().ok());
  EXPECT_GE(scheduler.lane_drains.load(), 1);
}

TEST(RuntimeCollisionMonitorTest, DestructionStillWaitsForTheWholeSharedLane) {
  FakeClock clock;
  LaneScheduler scheduler;
  HeldLaneWork held{clock, scheduler};
  ASSERT_TRUE(held.hold());
  auto monitor = std::make_unique<RuntimeCollisionMonitor>(
      clock, scheduler, RuntimeCollisionMonitor::Config{10'000'000},
      []() { return Result<void>::Ok(); }, []() { return true; });
  monitor->start();

  auto destroyed = std::async(std::launch::async, [&]() { monitor.reset(); });
  EXPECT_EQ(destroyed.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  held.release();
  destroyed.wait();
  EXPECT_GE(scheduler.lane_drains.load(), 1);
}

// OD-217: the schedule is anchored to the grid set by start(): a check that
// overruns two periods skips them (one callback, no catch-up burst) and the
// next check is due on the grid.
TEST(RuntimeCollisionMonitorTest, MissedPeriodsAreSkippedOnTheGridNotCaughtUp) {
  FakeClock clock;
  ManualScheduler scheduler;
  std::vector<TimePoint> started_at;
  RuntimeCollisionMonitor monitor{
      clock, scheduler, RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        started_at.push_back(clock.now());
        if (started_at.size() == 1U) clock.advance(25'000'000);  // ends at 35 ms
        return Result<void>::Ok();
      },
      []() { return true; }};
  monitor.start();  // grid 10, 20, 30, 40, ... ms
  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  ASSERT_EQ(started_at, (std::vector<TimePoint>{10'000'000}));
  // 20 and 30 ms were overrun: nothing runs for them.
  scheduler.advance_to(39'999'999);
  EXPECT_EQ(started_at.size(), 1U);
  clock.set(40'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(started_at, (std::vector<TimePoint>{10'000'000, 40'000'000}));
  clock.set(50'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(started_at.back(), 50'000'000);
  EXPECT_EQ(monitor.timing_metrics().missed_periods, 2U);
}

// OD-217: one report per run of late checks; an on-time check ends the run.
TEST(RuntimeCollisionMonitorTest, LateChecksReportOncePerEpisode) {
  FakeClock clock;
  ManualScheduler scheduler;
  std::vector<RuntimeCollisionMonitorLateEpisode> episodes;
  // Check durations per tick: late, late, on time, late.
  const std::vector<Duration> durations{12'000'000, 15'000'000, 1'000'000, 10'000'000};
  std::size_t tick = 0;
  RuntimeCollisionMonitor monitor{
      clock, scheduler, RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        clock.advance(durations.at(tick++));
        return Result<void>::Ok();
      },
      [&]() { return tick < durations.size(); },
      [&](const RuntimeCollisionMonitorLateEpisode& episode) { episodes.push_back(episode); }};
  monitor.start();
  for (int step = 0; step < 200 && tick < durations.size(); ++step) {
    clock.advance(1'000'000);
    scheduler.advance_to(clock.now());
  }
  ASSERT_EQ(tick, durations.size());
  ASSERT_EQ(episodes.size(), 2U);
  EXPECT_EQ(episodes[0].missed_periods, 1U);
  EXPECT_EQ(episodes[0].check_duration_ns, 12'000'000U);
  EXPECT_EQ(episodes[0].late_by_ns, 12'000'000U);
  EXPECT_EQ(episodes[0].totals.late_episodes, 1U);
  EXPECT_EQ(episodes[1].totals.late_episodes, 2U);
  EXPECT_EQ(episodes[1].totals.late_periods, 3U);
  const auto metrics = monitor.timing_metrics();
  EXPECT_EQ(metrics.late_episodes, 2U);
  EXPECT_EQ(metrics.late_periods, 3U);
  EXPECT_EQ(metrics.callback_duration_max_ns, 15'000'000U);
}

// OD-217: the deadman stamp is the newest verified sample time, never the
// time a check finished; start() opens the window at its own time.
TEST(RuntimeCollisionMonitorTest, StampIsTheVerifiedSampleTimeAndStartOpensTheWindow) {
  FakeClock clock;
  clock.set(1'000'000'000);
  ManualScheduler scheduler;
  TimePoint sample_received_ns = 0;
  RuntimeCollisionMonitor* monitor_ptr = nullptr;
  RuntimeCollisionMonitor monitor{
      clock, scheduler, RuntimeCollisionMonitor::Config{50'000'000},
      [&]() {
        clock.advance(70'000'000);  // a late, slow check
        monitor_ptr->note_verified_sample(sample_received_ns);
        return Result<void>::Ok();
      },
      []() { return true; }};
  monitor_ptr = &monitor;
  monitor.start();
  EXPECT_EQ(monitor.verified_sample_time_ns(), 1'000'000'000);

  // A sample received before the window opened leaves the window start.
  sample_received_ns = 990'000'000;
  clock.advance(50'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(monitor.verified_sample_time_ns(), 1'000'000'000);

  // The check finished at 1.19 s, but it verified a sample received at
  // 1.04 s: the stamp is 1.04 s.
  sample_received_ns = 1'040'000'000;
  clock.set(1'150'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(clock.now(), 1'220'000'000);
  EXPECT_EQ(monitor.verified_sample_time_ns(), 1'040'000'000);

  // An older sample never moves the stamp back; a new run restarts it.
  monitor.note_verified_sample(1'000'000'000);
  EXPECT_EQ(monitor.verified_sample_time_ns(), 1'040'000'000);
  monitor.stop(RuntimeCollisionMonitor::StopWait::kOwnCallbacks);
  monitor.start();
  EXPECT_EQ(monitor.verified_sample_time_ns(), clock.now());
}

// OD-217: robots have their own check lanes; one robot's held check does not
// delay another's, and the other's detach waits only for its own lane.
TEST(RuntimeCollisionMonitorTest, HeldCheckOfOneRobotDoesNotDelayAnotherRobotsLane) {
  FakeClock clock;
  LaneScheduler lane_a;
  LaneScheduler lane_b;
  HeldLaneWork held_a{clock, lane_a};
  ASSERT_TRUE(held_a.hold());

  std::atomic<int> ticks_b{0};
  auto monitor_b = std::make_unique<RuntimeCollisionMonitor>(
      clock, lane_b, RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        ++ticks_b;
        return Result<void>::Ok();
      },
      []() { return true; });
  monitor_b->start();
  auto driven = std::async(std::launch::async, [&]() {
    for (int i = 0; i < 3; ++i) {
      clock.advance(10'000'000);
      lane_b.inner.advance_to(clock.now());
    }
  });
  EXPECT_EQ(driven.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_EQ(ticks_b.load(), 3);

  auto detached = std::async(std::launch::async, [&]() { return monitor_b->detach(); });
  EXPECT_EQ(detached.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_TRUE(detached.get().ok());
  auto destroyed = std::async(std::launch::async, [&]() { monitor_b.reset(); });
  EXPECT_EQ(destroyed.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  held_a.release();
  driven.wait();
  destroyed.wait();
}

}  // namespace
}  // namespace manyforge::core
