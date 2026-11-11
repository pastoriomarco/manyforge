#include <gtest/gtest.h>

#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <manyforge_core/pi_mutex.hpp>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace manyforge::core {
namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// The waiter holds the lock until its condition variable releases it, so
// taking the lock once the waiter has announced itself proves it waits.
void AwaitBlockedWaiter(PiMutex& mutex, const std::atomic<bool>& waiting) {
  while (!waiting.load()) std::this_thread::yield();
  std::lock_guard lock(mutex);
}

TEST(PiMutexTest, RunsThePriorityInheritanceFutexProtocol) {
  // A PTHREAD_PRIO_INHERIT mutex stores its owner's thread id in the futex
  // word (the kernel boosts that owner); an ordinary glibc mutex stores 1.
  PiMutex mutex;
  std::lock_guard lock(mutex);
  EXPECT_EQ(static_cast<pid_t>(mutex.native_handle()->__data.__lock & FUTEX_TID_MASK),
            ::gettid());
}

TEST(PiMutexTest, LocksAndTryLocksLikeStdMutex) {
  PiMutex mutex;
  {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(lock.owns_lock());
    bool other_acquired = true;
    std::thread([&] { other_acquired = mutex.try_lock(); }).join();
    EXPECT_FALSE(other_acquired);
  }
  bool other_acquired = false;
  std::thread([&] {
    other_acquired = mutex.try_lock();
    if (other_acquired) mutex.unlock();
  }).join();
  EXPECT_TRUE(other_acquired);
  std::lock_guard guard(mutex);
}

TEST(PiMutexTest, TimedLockTimesOutWhileHeldAndAcquiresWhenFree) {
  PiMutex mutex;
  {
    std::lock_guard held(mutex);
    std::thread([&] {
      const auto started = Clock::now();
      EXPECT_FALSE(mutex.try_lock_for(milliseconds(20)));
      EXPECT_GE(Clock::now() - started, milliseconds(20));
      EXPECT_FALSE(mutex.try_lock_until(Clock::now() - milliseconds(1)));
      std::unique_lock timed(mutex, Clock::now() + milliseconds(5));
      EXPECT_FALSE(timed.owns_lock());
    }).join();
  }
  // Free: a past deadline still acquires, as std::timed_mutex does.
  EXPECT_TRUE(mutex.try_lock_until(Clock::now() - milliseconds(1)));
  mutex.unlock();
  std::unique_lock timed(mutex, milliseconds(5));
  EXPECT_TRUE(timed.owns_lock());
}

TEST(PiMutexTest, TimedLockAcquiresOnceReleasedBeforeTheDeadline) {
  PiMutex mutex;
  std::unique_lock held(mutex);
  std::atomic<bool> trying{false};
  bool acquired = false;
  std::thread waiter([&] {
    trying.store(true);
    acquired = mutex.try_lock_for(std::chrono::seconds(10));
    if (acquired) mutex.unlock();
  });
  while (!trying.load()) std::this_thread::yield();
  held.unlock();
  waiter.join();
  EXPECT_TRUE(acquired);
}

TEST(PiConditionVariableTest, WaitReturnsOnNotify) {
  PiMutex mutex;
  PiConditionVariable condition;
  std::atomic<bool> waiting{false};
  bool ready = false;
  std::thread notifier([&] {
    AwaitBlockedWaiter(mutex, waiting);
    {
      std::lock_guard lock(mutex);
      ready = true;
    }
    condition.notify_one();
  });
  std::unique_lock lock(mutex);
  waiting.store(true);
  condition.wait(lock, [&] { return ready; });
  EXPECT_TRUE(ready);
  EXPECT_TRUE(lock.owns_lock());
  lock.unlock();
  notifier.join();
}

TEST(PiConditionVariableTest, TimedWaitsReturnOnNotifyAndOnTimeout) {
  PiMutex mutex;
  PiConditionVariable condition;
  std::unique_lock lock(mutex);
  auto started = Clock::now();
  EXPECT_EQ(condition.wait_for(lock, milliseconds(20)), std::cv_status::timeout);
  EXPECT_GE(Clock::now() - started, milliseconds(20));
  started = Clock::now();
  EXPECT_FALSE(condition.wait_until(lock, started + milliseconds(20), [] { return false; }));
  EXPECT_GE(Clock::now() - started, milliseconds(20));
  EXPECT_TRUE(lock.owns_lock());

  std::atomic<bool> waiting{false};
  bool ready = false;
  std::thread notifier([&] {
    AwaitBlockedWaiter(mutex, waiting);
    {
      std::lock_guard notify_lock(mutex);
      ready = true;
    }
    condition.notify_all();
  });
  waiting.store(true);
  EXPECT_TRUE(condition.wait_for(lock, std::chrono::seconds(10), [&] { return ready; }));
  lock.unlock();
  notifier.join();
}

TEST(PiConditionVariableTest, StopTokenWaitReturnsOnStop) {
  PiMutex mutex;
  PiConditionVariable condition;
  std::stop_source stop;
  std::atomic<bool> waiting{false};
  std::thread stopper([&] {
    AwaitBlockedWaiter(mutex, waiting);
    stop.request_stop();
  });
  std::unique_lock lock(mutex);
  waiting.store(true);
  EXPECT_FALSE(condition.wait(lock, stop.get_token(), [] { return false; }));
  EXPECT_TRUE(lock.owns_lock());
  lock.unlock();
  stopper.join();
}

// As with std::condition_variable_any, the stop callback takes only the
// condition variable's internal mutex: a stop requested while holding the
// waited lock does not hang, and the waiter returns once the lock is free.
TEST(PiConditionVariableTest, StopRequestedWhileHoldingTheWaitedLockDoesNotHang) {
  PiMutex mutex;
  PiConditionVariable condition;
  for (const bool timed : {false, true}) {
    std::stop_source stop;
    std::atomic<bool> waiting{false};
    std::thread stopper([&] {
      while (!waiting.load()) std::this_thread::yield();
      std::lock_guard held(mutex);  // only once the waiter is in its wait
      stop.request_stop();
    });
    std::unique_lock lock(mutex);
    waiting.store(true);
    const bool result =
        timed ? condition.wait_for(lock, stop.get_token(), std::chrono::seconds(10),
                                   [] { return false; })
              : condition.wait(lock, stop.get_token(), [] { return false; });
    EXPECT_FALSE(result);
    EXPECT_TRUE(lock.owns_lock());
    lock.unlock();
    stopper.join();
  }
}

TEST(PiConditionVariableTest, StopTokenTimedWaitReturnsOnStopNotifyAndTimeout) {
  PiMutex mutex;
  PiConditionVariable condition;
  std::unique_lock lock(mutex);

  // Timeout.
  std::stop_source idle;
  auto started = Clock::now();
  EXPECT_FALSE(condition.wait_for(lock, idle.get_token(), milliseconds(20), [] { return false; }));
  EXPECT_GE(Clock::now() - started, milliseconds(20));

  // Already stopped: pred() at once.
  std::stop_source stopped;
  stopped.request_stop();
  EXPECT_TRUE(condition.wait_until(lock, stopped.get_token(), Clock::now() + std::chrono::seconds(10),
                                   [] { return true; }));
  EXPECT_FALSE(condition.wait_until(lock, stopped.get_token(),
                                    Clock::now() + std::chrono::seconds(10), [] { return false; }));

  // Stop during the wait.
  std::stop_source stop;
  std::atomic<bool> waiting{false};
  std::thread stopper([&] {
    AwaitBlockedWaiter(mutex, waiting);
    stop.request_stop();
  });
  waiting.store(true);
  started = Clock::now();
  EXPECT_FALSE(condition.wait_for(lock, stop.get_token(), std::chrono::seconds(10),
                                  [] { return false; }));
  EXPECT_LT(Clock::now() - started, std::chrono::seconds(10));
  EXPECT_TRUE(lock.owns_lock());
  stopper.join();

  // Notify during the wait.
  std::stop_source unused;
  bool ready = false;
  waiting.store(false);
  std::thread notifier([&] {
    AwaitBlockedWaiter(mutex, waiting);
    {
      std::lock_guard notify_lock(mutex);
      ready = true;
    }
    condition.notify_one();
  });
  waiting.store(true);
  EXPECT_TRUE(condition.wait_for(lock, unused.get_token(), std::chrono::seconds(10),
                                 [&] { return ready; }));
  EXPECT_TRUE(lock.owns_lock());
  lock.unlock();
  notifier.join();
}

// Rule 3, "never slower": uncontended cost against the standard types on
// this machine. Reported, not asserted: the figures belong to the release
// measurement, and a threshold here would be a guess.
template <class Body>
double NanosecondsPerOperation(int iterations, Body body) {
  const auto started = Clock::now();
  for (int i = 0; i < iterations; ++i) body();
  return std::chrono::duration<double, std::nano>(Clock::now() - started).count() / iterations;
}

TEST(PiLockCostTest, ReportsUncontendedLockAndWaitCostAgainstStdTypes) {
  constexpr int kLockIterations = 200000;
  constexpr int kWaitIterations = 2000;
  std::mutex std_mutex;
  std::condition_variable std_condition;
  PiMutex pi_mutex;
  PiConditionVariable pi_condition;

  // Warm-up, so the first measured loop does not pay for frequency ramp-up.
  (void)NanosecondsPerOperation(kLockIterations, [&] {
    std_mutex.lock();
    std_mutex.unlock();
    pi_mutex.lock();
    pi_mutex.unlock();
  });
  const double std_lock = NanosecondsPerOperation(kLockIterations, [&] {
    std_mutex.lock();
    std_mutex.unlock();
  });
  const double pi_lock = NanosecondsPerOperation(kLockIterations, [&] {
    pi_mutex.lock();
    pi_mutex.unlock();
  });
  const double std_notify =
      NanosecondsPerOperation(kLockIterations, [&] { std_condition.notify_one(); });
  const double pi_notify =
      NanosecondsPerOperation(kLockIterations, [&] { pi_condition.notify_one(); });
  // A complete wait (enqueue, release, return, reacquire) with a deadline
  // already passed, so no second thread is involved.
  std::unique_lock std_lock_held(std_mutex);
  const double std_wait = NanosecondsPerOperation(kWaitIterations, [&] {
    (void)std_condition.wait_until(std_lock_held, Clock::now());
  });
  std_lock_held.unlock();
  std::unique_lock pi_lock_held(pi_mutex);
  const double pi_wait = NanosecondsPerOperation(kWaitIterations, [&] {
    (void)pi_condition.wait_until(pi_lock_held, Clock::now());
  });
  pi_lock_held.unlock();

  std::printf(
      "[pi-lock-cost] ns/op std vs pi: lock+unlock %.1f vs %.1f; notify_one %.1f vs %.1f; "
      "expired wait_until %.1f vs %.1f\n",
      std_lock, pi_lock, std_notify, pi_notify, std_wait, pi_wait);
  RecordProperty("std_lock_unlock_ns", std::to_string(std_lock));
  RecordProperty("pi_lock_unlock_ns", std::to_string(pi_lock));
  RecordProperty("std_notify_one_ns", std::to_string(std_notify));
  RecordProperty("pi_notify_one_ns", std::to_string(pi_notify));
  RecordProperty("std_expired_wait_ns", std::to_string(std_wait));
  RecordProperty("pi_expired_wait_ns", std::to_string(pi_wait));
}

// Inheritance under SCHED_FIFO on one CPU: a low-priority holder, a
// medium-priority spinner and a high-priority waiter. With inheritance the
// holder runs at the waiter's priority, finishes its short hold and hands
// over; without it the spinner starves the holder until the spinner stops.
constexpr int kLowPriority = 10;
constexpr int kMediumPriority = 20;
constexpr int kHighPriority = 30;
constexpr auto kHold = milliseconds(10);        // holder CPU time under the lock
constexpr auto kSpin = milliseconds(500);       // spinner's wall-clock limit
constexpr auto kInheritedBound = milliseconds(250);
constexpr auto kSetupLimit = std::chrono::seconds(5);

bool BecomeFifoOn(int priority, int cpu) {
  // Priority first: pinned as SCHED_OTHER, the thread would be starved by
  // the FIFO threads already on that CPU before it could raise itself.
  sched_param param{};
  param.sched_priority = priority;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

std::chrono::nanoseconds ThreadCpuTime() {
  timespec now{};
  ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return std::chrono::seconds(now.tv_sec) + std::chrono::nanoseconds(now.tv_nsec);
}

bool AwaitFlag(const std::atomic<bool>& flag) {
  const auto limit = Clock::now() + kSetupLimit;
  while (!flag.load()) {
    if (Clock::now() > limit) return false;
    std::this_thread::yield();
  }
  return true;
}

// The high-priority waiter's time to acquire, or nullopt when the threads
// could not be set up.
template <class Mutex>
std::optional<Clock::duration> HighWaiterAcquireTime(int cpu) {
  Mutex mutex;
  std::atomic<bool> failed{false};
  std::atomic<bool> low_holds{false};
  std::atomic<bool> medium_runs{false};
  std::atomic<bool> high_done{false};
  Clock::duration acquire{};

  std::thread low([&] {
    if (!BecomeFifoOn(kLowPriority, cpu)) {
      failed.store(true);
      return;
    }
    std::lock_guard lock(mutex);
    low_holds.store(true);
    const auto until = ThreadCpuTime() + kHold;
    while (ThreadCpuTime() < until && !failed.load()) {
    }
  });
  if (!AwaitFlag(low_holds) || failed.load()) {
    failed.store(true);
    low.join();
    return std::nullopt;
  }
  std::thread medium([&] {
    if (!BecomeFifoOn(kMediumPriority, cpu)) {
      failed.store(true);
      return;
    }
    medium_runs.store(true);
    const auto until = Clock::now() + kSpin;
    while (!high_done.load() && !failed.load() && Clock::now() < until) {
    }
  });
  if (!AwaitFlag(medium_runs) || failed.load()) {
    failed.store(true);
    medium.join();
    low.join();
    return std::nullopt;
  }
  std::thread high([&] {
    if (!BecomeFifoOn(kHighPriority, cpu)) {
      failed.store(true);
      return;
    }
    const auto started = Clock::now();
    mutex.lock();
    acquire = Clock::now() - started;
    mutex.unlock();
    high_done.store(true);
  });
  high.join();
  medium.join();
  low.join();
  if (failed.load()) return std::nullopt;
  return acquire;
}

bool FifoAllowed() {
  bool allowed = false;
  std::thread([&] {
    sched_param param{};
    param.sched_priority = kHighPriority;
    allowed = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
  }).join();
  return allowed;
}

TEST(PiMutexInheritanceTest, HighPriorityWaiterIsNotDelayedByAMediumPrioritySpinner) {
  if (!FifoAllowed())
    GTEST_SKIP() << "SCHED_FIFO needs CAP_SYS_NICE or an RLIMIT_RTPRIO of " << kHighPriority;
  cpu_set_t inherited;
  CPU_ZERO(&inherited);
  ASSERT_EQ(pthread_getaffinity_np(pthread_self(), sizeof(inherited), &inherited), 0);
  if (CPU_COUNT(&inherited) < 2) GTEST_SKIP() << "needs two CPUs: one for the test, one for main";
  int test_cpu = -1;
  int main_cpu = -1;
  for (int cpu = 0; cpu < CPU_SETSIZE && main_cpu < 0; ++cpu) {
    if (!CPU_ISSET(cpu, &inherited)) continue;
    (test_cpu < 0 ? test_cpu : main_cpu) = cpu;
  }
  // Keep this thread off the test CPU, where the FIFO spinner would starve it.
  cpu_set_t main_set;
  CPU_ZERO(&main_set);
  CPU_SET(main_cpu, &main_set);
  ASSERT_EQ(pthread_setaffinity_np(pthread_self(), sizeof(main_set), &main_set), 0);

  // The control shows that this setup separates the two cases here.
  const auto without = HighWaiterAcquireTime<std::mutex>(test_cpu);
  const auto with = HighWaiterAcquireTime<PiMutex>(test_cpu);
  ASSERT_EQ(pthread_setaffinity_np(pthread_self(), sizeof(inherited), &inherited), 0);
  ASSERT_TRUE(without.has_value());
  ASSERT_TRUE(with.has_value());
  const auto ms = [](Clock::duration value) {
    return std::chrono::duration<double, std::milli>(value).count();
  };
  std::printf("[pi-inheritance] high waiter acquire ms: std::mutex %.2f, PiMutex %.2f\n",
              ms(*without), ms(*with));
  EXPECT_GE(*without, kInheritedBound) << "the setup did not starve the holder";
  EXPECT_LT(*with, kInheritedBound);
}

}  // namespace
}  // namespace manyforge::core
