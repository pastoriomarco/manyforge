#include <gtest/gtest.h>

#include "../src/execution_motion_target_limits_internal.hpp"

#include <manyforge_roboplan/execution_host_support.hpp>
#include <manyforge_roboplan/retained_view_predecessor.hpp>
#include <manyforge_roboplan/writer_preferring_shared_gate.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <map>
#include <new>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <cstdlib>

#ifdef __linux__
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif


// Counts this thread's allocations while enabled (rule 4: a time-critical
// path's allocation count may not grow).
namespace {
thread_local bool count_allocations = false;
thread_local std::size_t counted_allocations = 0U;
void* CountedAllocate(const std::size_t size) {
  if (count_allocations) ++counted_allocations;
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) return memory;
  throw std::bad_alloc();
}
// Out of line: see test_bounded_stop_control_executor.cpp.
[[gnu::noinline]] void CountedDeallocate(void* memory) noexcept { std::free(memory); }
template <typename Call>
std::size_t AllocationsDuring(Call&& call) {
  counted_allocations = 0U;
  count_allocations = true;
  call();
  count_allocations = false;
  return counted_allocations;
}
}  // namespace
void* operator new(const std::size_t size) { return CountedAllocate(size); }
void* operator new[](const std::size_t size) { return CountedAllocate(size); }
void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  if (count_allocations) ++counted_allocations;
  return std::malloc(size == 0U ? 1U : size);
}
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  if (count_allocations) ++counted_allocations;
  return std::malloc(size == 0U ? 1U : size);
}
void operator delete(void* memory) noexcept { CountedDeallocate(memory); }
void operator delete[](void* memory) noexcept { CountedDeallocate(memory); }
void operator delete(void* memory, std::size_t) noexcept { CountedDeallocate(memory); }
void operator delete[](void* memory, std::size_t) noexcept { CountedDeallocate(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { CountedDeallocate(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { CountedDeallocate(memory); }

using namespace manyforge::roboplan_backend;
using namespace manyforge::core;

namespace {

// Lane-isolation tests first park the blocking roles and only then schedule
// the lane under test, so "fires while blocked" holds by construction rather
// than by a 10 ms head start that a loaded machine can overrun.
template <typename Predicate>
bool wait_until_true(Predicate predicate,
                     std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// Releases parked callbacks on every exit path (including a failed ASSERT)
// so the scheduler's worker threads can be joined.
struct ReleaseOnExit {
  std::atomic<bool>& release;
  ~ReleaseOnExit() { release.store(true); }
};

}  // namespace

TEST(NativeSchedulerTest, CloseAdmissionDiscardsQueuedWorkAndRejectsNewWork) {
  SimpleScheduler scheduler;
  std::atomic<bool> fired{false};
  const auto future = std::chrono::duration_cast<std::chrono::nanoseconds>(
      (std::chrono::steady_clock::now() + std::chrono::hours(1)).time_since_epoch()).count();
  scheduler.schedule(future, [&] { fired.store(true); });
  scheduler.close_admission_and_discard();
  EXPECT_TRUE(scheduler.drain_all_for(std::chrono::milliseconds(100)));
  EXPECT_FALSE(fired.load());
  EXPECT_THROW(scheduler.schedule(future, [] {}), std::runtime_error);
}

TEST(NativeSchedulerTest, CancelReleasesCapturedOwnerOutsideWorkerLock) {
  SimpleScheduler scheduler;
  const auto future = std::chrono::duration_cast<std::chrono::nanoseconds>(
      (std::chrono::steady_clock::now() + std::chrono::hours(1)).time_since_epoch()).count();
  bool released = false;
  struct Owner {
    std::function<void()> on_destroy;
    ~Owner() { on_destroy(); }
  };
  auto owner = std::make_shared<Owner>();
  owner->on_destroy = [&] {
    scheduler.schedule(future, [] {});
    released = true;
  };
  const auto handle = scheduler.schedule(future, [owner] {});
  owner.reset();
  EXPECT_TRUE(scheduler.cancel_before_dequeue(handle));
  EXPECT_TRUE(released);
  scheduler.close_admission_and_discard();
}

TEST(NativeSchedulerTest, LaneRunsByDeadlineThenScheduleOrderAndCancelRemovesOneTask) {
  SimpleScheduler scheduler;
  std::atomic<bool> parked{false};
  std::atomic<bool> release{false};
  ReleaseOnExit release_on_exit{release};
  std::mutex order_mutex;
  std::vector<int> order;
  const auto record = [&](int value) {
    return [&, value] {
      std::lock_guard lock(order_mutex);
      order.push_back(value);
    };
  };
  // Park the lane so every task below is queued before the worker picks one.
  scheduler.schedule(0, [&] {
    parked.store(true);
    while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  ASSERT_TRUE(wait_until_true([&] { return parked.load(); }));
  scheduler.schedule(30, record(30));
  scheduler.schedule(10, record(11));
  const auto cancelled = scheduler.schedule(20, record(20));
  scheduler.schedule(10, record(12));
  scheduler.schedule(20, record(21));
  scheduler.schedule(5, record(5));
  EXPECT_TRUE(scheduler.cancel_before_dequeue(cancelled));
  EXPECT_FALSE(scheduler.cancel_before_dequeue(cancelled));
  release.store(true);
  // A drain waits only for a running callback, not for queued tasks: wait for
  // the five that remain (the cancelled one never runs).
  ASSERT_TRUE(wait_until_true([&] {
    std::lock_guard lock(order_mutex);
    return order.size() == 5U;
  }));
  std::lock_guard lock(order_mutex);
  EXPECT_EQ(order, (std::vector<int>{5, 11, 12, 21, 30}));
}

TEST(NativeSchedulerTest, ScheduleAndCancelAllocationCount) {
  SimpleScheduler scheduler;
  const auto future = std::chrono::duration_cast<std::chrono::nanoseconds>(
      (std::chrono::steady_clock::now() + std::chrono::hours(1)).time_since_epoch()).count();
  // Start the lane first so only the queue work is counted.
  scheduler.cancel(scheduler.schedule(future, [] {}));
  for (int round = 0; round < 3; ++round) {
    std::function<void()> callback = [] {};
    ScheduleHandle handle{};
    const auto scheduled =
        AllocationsDuring([&] { handle = scheduler.schedule(future, std::move(callback)); });
    const auto cancelled = AllocationsDuring([&] { scheduler.cancel(handle); });
    // Reported for the before/after comparison.
    std::cout << "schedule allocations " << scheduled << " cancel allocations " << cancelled
              << '\n';
    EXPECT_EQ(cancelled, 0U);
  }
  scheduler.close_admission_and_discard();
}

TEST(NativeSchedulerTest, SimpleSchedulerHonorsFutureDeadline) {
  SimpleScheduler scheduler;
  std::atomic<bool> fired{false};

  const auto now = std::chrono::steady_clock::now();
  const auto deadline =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          now.time_since_epoch())
          .count() +
      50'000'000LL;

  scheduler.schedule(deadline, [&fired]() { fired.store(true); });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(fired.load());

  const auto timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (!fired.load() && std::chrono::steady_clock::now() < timeout) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(fired.load());
}

TEST(NativeSchedulerTest, MonitoringLaneFiresWhileRuntimeSafetyLaneIsBlocked) {
  // Audit K3 regression: with the previous single-worker scheduler a
  // long-blocking runtime-safety callback (e.g. collision_check_tick stuck
  // waiting on the scene mutex) would prevent the Monitoring watchdog
  // from firing — including the watchdog whose job is to abort that
  // stall. With the split scheduler the lanes are independent.
  SimpleScheduler scheduler;
  std::atomic<bool> runtime_safety_can_exit{false};
  std::atomic<bool> runtime_safety_running{false};
  std::atomic<bool> monitoring_fired{false};
  ReleaseOnExit release_on_exit{runtime_safety_can_exit};

  const auto schedule_at = [&](int64_t offset_ns) {
    const auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return now_ns + offset_ns;
  };

  // Runtime-safety callback parks itself until the test releases it; mirrors
  // a collision_check_tick stalled on `scene_mutex_` during a long RRT.
  scheduler.schedule(
      schedule_at(10'000'000LL),
      [&]() {
        runtime_safety_running.store(true);
        while (!runtime_safety_can_exit.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      },
      SchedulerPriority::RuntimeSafety);

  ASSERT_TRUE(wait_until_true([&]() { return runtime_safety_running.load(); }))
      << "Runtime-safety callback did not reach its parking loop";

  // Monitoring callback must fire even while Safety is parked.
  scheduler.schedule(
      schedule_at(10'000'000LL),
      [&]() { monitoring_fired.store(true); },
      SchedulerPriority::Monitoring);

  // Wait up to 500 ms for the watchdog to fire under contention.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!monitoring_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(monitoring_fired.load())
      << "Monitoring watchdog did not fire while runtime-safety lane was blocked";

  // Release the parked runtime-safety callback so the scheduler destructor
  // does not race with a still-running task.
  runtime_safety_can_exit.store(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

TEST(NativeSchedulerTest, ControllerCommandLaneFiresWhileOtherRolesAreBlocked) {
  SimpleScheduler scheduler;
  std::atomic<bool> release_blocked_roles{false};
  std::atomic<int> blocked_roles_running{0};
  std::atomic<bool> controller_fired{false};
  ReleaseOnExit release_on_exit{release_blocked_roles};

  const auto schedule_at = [&](int64_t offset_ns) {
    const auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return now_ns + offset_ns;
  };

  const auto block_role = [&](SchedulerPriority priority) {
    scheduler.schedule(
        schedule_at(10'000'000LL),
        [&]() {
          blocked_roles_running.fetch_add(1);
          while (!release_blocked_roles.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        },
        priority);
  };

  block_role(SchedulerPriority::RuntimeSafety);
  block_role(SchedulerPriority::StopControl);
  block_role(SchedulerPriority::Lifecycle);
  block_role(SchedulerPriority::Monitoring);
  block_role(SchedulerPriority::Background);
  ASSERT_TRUE(wait_until_true([&]() { return blocked_roles_running.load() == 5; }))
      << "only " << blocked_roles_running.load() << " of 5 blocking roles parked";

  scheduler.schedule(
      schedule_at(10'000'000LL),
      [&]() { controller_fired.store(true); },
      SchedulerPriority::ControllerCommand);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!controller_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(controller_fired.load())
      << "Controller-command lane was starved by blocked lower-priority roles";

  release_blocked_roles.store(true);
  EXPECT_TRUE(scheduler.drain_all_for(std::chrono::seconds(1)));
}

TEST(NativeSchedulerTest, RuntimeSafetyLaneFiresWhileLifecycleLaneIsBlocked) {
  SimpleScheduler scheduler;
  std::atomic<bool> release_lifecycle{false};
  std::atomic<bool> lifecycle_running{false};
  std::atomic<bool> runtime_safety_fired{false};
  ReleaseOnExit release_on_exit{release_lifecycle};

  const auto schedule_at = [&](int64_t offset_ns) {
    const auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return now_ns + offset_ns;
  };

  scheduler.schedule(
      schedule_at(10'000'000LL),
      [&]() {
        lifecycle_running.store(true);
        while (!release_lifecycle.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      },
      SchedulerPriority::Lifecycle);
  ASSERT_TRUE(wait_until_true([&]() { return lifecycle_running.load(); }))
      << "Lifecycle callback did not reach its parking loop";
  scheduler.schedule(
      schedule_at(10'000'000LL),
      [&]() { runtime_safety_fired.store(true); },
      SchedulerPriority::RuntimeSafety);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!runtime_safety_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(runtime_safety_fired.load())
      << "Runtime-safety lane was starved by blocked lifecycle work";

  release_lifecycle.store(true);
  EXPECT_TRUE(scheduler.drain_all_for(std::chrono::seconds(1)));
}

TEST(NativeSchedulerTest, StopControlLaneFiresWhileLowerRolesAreBlocked) {
  SimpleScheduler scheduler;
  std::atomic<bool> release_blocked_roles{false};
  std::atomic<int> blocked_roles_running{0};
  std::atomic<bool> stop_control_fired{false};
  ReleaseOnExit release_on_exit{release_blocked_roles};

  const auto schedule_at = [&](int64_t offset_ns) {
    const auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return now_ns + offset_ns;
  };
  const auto block_role = [&](SchedulerPriority priority) {
    scheduler.schedule(
        schedule_at(10'000'000LL),
        [&]() {
          blocked_roles_running.fetch_add(1);
          while (!release_blocked_roles.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        },
        priority);
  };

  block_role(SchedulerPriority::Lifecycle);
  block_role(SchedulerPriority::Monitoring);
  block_role(SchedulerPriority::Background);
  ASSERT_TRUE(wait_until_true([&]() { return blocked_roles_running.load() == 3; }))
      << "only " << blocked_roles_running.load() << " of 3 blocking roles parked";

  scheduler.schedule(
      schedule_at(10'000'000LL),
      [&]() { stop_control_fired.store(true); },
      SchedulerPriority::StopControl);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!stop_control_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(stop_control_fired.load())
      << "Stop-control lane was starved by blocked lower-priority roles";

  release_blocked_roles.store(true);
  EXPECT_TRUE(scheduler.drain_all_for(std::chrono::seconds(1)));
}

TEST(NativeSchedulerTest, StrictEnvironmentProfileIsExplicitAndOrdered) {
#ifdef __linux__
  const std::map<std::string, std::string> environment = {
      {"MANYFORGE_RT_RESOURCE_PROFILE", "strict"},
      {"MANYFORGE_RT_SCHED_POLICY", "FIFO"},
      {"MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY", "45"},
      {"MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY", "4"},
      {"MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY", "40"},
      {"MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY", "5-6"},
      {"MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY", "7"},
  };

  const auto config = SimpleSchedulerConfig::from_environment(environment);
  const auto& controller =
      config.workers[scheduler_priority_index(SchedulerPriority::ControllerCommand)];
  const auto& safety =
      config.workers[scheduler_priority_index(SchedulerPriority::RuntimeSafety)];
  const auto& stop =
      config.workers[scheduler_priority_index(SchedulerPriority::StopControl)];

  ASSERT_TRUE(controller.policy.has_value());
  EXPECT_EQ(*controller.policy, SCHED_FIFO);
  EXPECT_EQ(controller.priority, 45);
  EXPECT_EQ(controller.cpu_affinity, std::vector<int>({4}));
  EXPECT_TRUE(controller.required);
  EXPECT_EQ(safety.priority, 40);
  EXPECT_EQ(safety.cpu_affinity, std::vector<int>({5, 6}));
  // OD-58: StopControl is the highest lane by default, on its own CPUs.
  EXPECT_EQ(stop.priority, 48);
  EXPECT_GT(stop.priority, controller.priority);
  EXPECT_EQ(stop.cpu_affinity, std::vector<int>({7}));
  // OD-59: the strict profile locks the process memory and pre-faults the
  // three priority workers' stacks.
  EXPECT_TRUE(config.lock_memory);
  EXPECT_TRUE(controller.prefault_stack);
  EXPECT_TRUE(safety.prefault_stack);
  EXPECT_TRUE(stop.prefault_stack);
  EXPECT_FALSE(
      config.workers[scheduler_priority_index(SchedulerPriority::Lifecycle)].prefault_stack);
  EXPECT_FALSE(SimpleSchedulerConfig::from_environment({}).lock_memory);
  EXPECT_FALSE(
      config.workers[scheduler_priority_index(SchedulerPriority::Lifecycle)]
          .requested());
#else
  GTEST_SKIP() << "strict scheduler profiles are Linux-only";
#endif
}

TEST(NativeSchedulerTest, StrictEnvironmentProfileRejectsMissingSafetyCpu) {
#ifdef __linux__
  const std::map<std::string, std::string> environment = {
      {"MANYFORGE_RT_RESOURCE_PROFILE", "strict"},
      {"MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY", "2"},
  };
  EXPECT_THROW(
      (void)SimpleSchedulerConfig::from_environment(environment),
      std::invalid_argument);
#else
  GTEST_SKIP() << "strict scheduler profiles are Linux-only";
#endif
}

TEST(NativeSchedulerTest, StrictEnvironmentProfileRejectsUnboundedCpuRange) {
#ifdef __linux__
  const std::map<std::string, std::string> environment = {
      {"MANYFORGE_RT_RESOURCE_PROFILE", "strict"},
      {"MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY", "0-2147483647"},
      {"MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY", "1"},
  };
  EXPECT_THROW(
      (void)SimpleSchedulerConfig::from_environment(environment),
      std::invalid_argument);
#else
  GTEST_SKIP() << "strict scheduler profiles are Linux-only";
#endif
}

TEST(NativeSchedulerTest, StrictEnvironmentProfileRejectsInvertedPriorities) {
#ifdef __linux__
  const std::map<std::string, std::string> environment = {
      {"MANYFORGE_RT_RESOURCE_PROFILE", "strict"},
      {"MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY", "40"},
      {"MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY", "4"},
      {"MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY", "45"},
      {"MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY", "5"},
      {"MANYFORGE_RT_STOP_CONTROL_PRIORITY", "48"},
      {"MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY", "6"},
  };
  EXPECT_THROW(
      (void)SimpleSchedulerConfig::from_environment(environment),
      std::invalid_argument);
#else
  GTEST_SKIP() << "strict scheduler profiles are Linux-only";
#endif
}

// OD-58: StopControl gets no ControllerCommand CPUs by default (its CPUs
// are required), and a StopControl priority at or below ControllerCommand
// is refused.
TEST(NativeSchedulerTest, StrictEnvironmentProfileNeedsAHighestPriorityStopLaneWithItsOwnCpus) {
#ifdef __linux__
  std::map<std::string, std::string> environment = {
      {"MANYFORGE_RT_RESOURCE_PROFILE", "strict"},
      {"MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY", "4"},
      {"MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY", "5"},
  };
  EXPECT_THROW((void)SimpleSchedulerConfig::from_environment(environment),
               std::invalid_argument);
  environment["MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY"] = "6";
  EXPECT_NO_THROW((void)SimpleSchedulerConfig::from_environment(environment));
  environment["MANYFORGE_RT_STOP_CONTROL_PRIORITY"] = "45";
  EXPECT_THROW((void)SimpleSchedulerConfig::from_environment(environment),
               std::invalid_argument);
#else
  GTEST_SKIP() << "strict scheduler profiles are Linux-only";
#endif
}

// OD-59: a memory lock the OS refuses (here a zero memlock limit) refuses
// the strict scheduler's construction with its error, like a rejected
// worker profile; nothing crashes. Run in a child so the test process keeps
// its own limits and memory unlocked.
TEST(NativeSchedulerDeathTest, StrictMemoryLockRefusalRefusesConstruction) {
#ifdef __linux__
  if (::geteuid() == 0) GTEST_SKIP() << "root may lock memory past RLIMIT_MEMLOCK";
  EXPECT_EXIT(
      {
        rlimit none{};  // both limits zero (no comma: EXPECT_EXIT is a macro)
        if (::setrlimit(RLIMIT_MEMLOCK, &none) != 0) std::_Exit(2);
        SimpleSchedulerConfig config;
        config.lock_memory = true;
        try {
          SimpleScheduler scheduler(std::move(config));
        } catch (const std::runtime_error& error) {
          const std::string text = error.what();
          std::_Exit(text.find("rejected its memory lock") != std::string::npos ? 3 : 4);
        }
        std::_Exit(0);
      },
      ::testing::ExitedWithCode(3), "");
#else
  GTEST_SKIP() << "strict scheduler profiles are Linux-only";
#endif
}

TEST(NativeSchedulerTest, StrictEnvironmentProfileRejectsSafetyCpuOverlap) {
#ifdef __linux__
  const std::map<std::string, std::string> environment = {
      {"MANYFORGE_RT_RESOURCE_PROFILE", "strict"},
      {"MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY", "4"},
      {"MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY", "4"},
      {"MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY", "6"},
  };
  EXPECT_THROW(
      (void)SimpleSchedulerConfig::from_environment(environment),
      std::invalid_argument);
#else
  GTEST_SKIP() << "strict scheduler profiles are Linux-only";
#endif
}

TEST(NativeSchedulerTest, RequiredWorkerAffinityIsAppliedBeforeCallbacks) {
#ifdef __linux__
  cpu_set_t inherited_set;
  CPU_ZERO(&inherited_set);
  ASSERT_EQ(sched_getaffinity(0, sizeof(inherited_set), &inherited_set), 0);
  int selected_cpu = -1;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &inherited_set)) {
      selected_cpu = cpu;
      break;
    }
  }
  ASSERT_GE(selected_cpu, 0);

  SimpleSchedulerConfig config;
  auto& background =
      config.workers[scheduler_priority_index(SchedulerPriority::Background)];
  background.policy = SCHED_OTHER;
  background.priority = 0;
  background.cpu_affinity = {selected_cpu};
  background.required = true;
  SimpleScheduler scheduler(std::move(config));

  std::promise<std::pair<int, bool>> observed;
  auto observed_future = observed.get_future();
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  scheduler.schedule(
      now_ns,
      [&]() {
        cpu_set_t callback_set;
        CPU_ZERO(&callback_set);
        const bool exact_affinity =
            sched_getaffinity(0, sizeof(callback_set), &callback_set) == 0 &&
            CPU_COUNT(&callback_set) == 1 &&
            CPU_ISSET(selected_cpu, &callback_set);
        observed.set_value({sched_getscheduler(0), exact_affinity});
      },
      SchedulerPriority::Background);

  ASSERT_EQ(observed_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  const auto [policy, exact_affinity] = observed_future.get();
  EXPECT_EQ(policy, SCHED_OTHER);
  EXPECT_TRUE(exact_affinity);
#else
  GTEST_SKIP() << "worker affinity is Linux-only";
#endif
}

TEST(NativeSchedulerTest, RequiredInvalidAffinityFailsConstructionClosed) {
#ifdef __linux__
  SimpleSchedulerConfig config;
  auto& controller =
      config.workers[scheduler_priority_index(SchedulerPriority::ControllerCommand)];
  controller.policy = SCHED_OTHER;
  controller.priority = 0;
  controller.cpu_affinity = {CPU_SETSIZE};
  controller.required = true;
  EXPECT_THROW(SimpleScheduler scheduler(std::move(config)), std::runtime_error);
#else
  GTEST_SKIP() << "worker affinity is Linux-only";
#endif
}

TEST(NativeSchedulerTest, DestructionWakesParkedWorkersOnAllRoles) {
  auto scheduler = std::make_unique<SimpleScheduler>();
  const auto far_future =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          (std::chrono::steady_clock::now() + std::chrono::seconds(60))
              .time_since_epoch())
          .count();
  for (std::size_t i = 0; i < kSchedulerPriorityCount; ++i) {
    scheduler->schedule(
        far_future,
        []() { ADD_FAILURE() << "far-future callback fired during teardown"; },
        scheduler_priority_at(i));
  }

  auto destroy_future = std::async(std::launch::async, [&]() {
    scheduler.reset();
  });
  EXPECT_EQ(destroy_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
}

TEST(NativeSchedulerTest, BackgroundCallbackExceptionDoesNotStopWorker) {
  SimpleScheduler scheduler;
  std::atomic<bool> followup_fired{false};

  const auto now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  scheduler.schedule(
      now_ns,
      []() { throw std::runtime_error("background diagnostic failed"); },
      SchedulerPriority::Background);
  scheduler.schedule(
      now_ns + 1'000'000LL,
      [&]() { followup_fired.store(true); },
      SchedulerPriority::Background);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!followup_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(followup_fired.load())
      << "Background worker stopped after a contained callback exception";
  EXPECT_TRUE(
      scheduler.drain_for(SchedulerPriority::Background, std::chrono::seconds(1)));
}

TEST(NativeSchedulerTest,
     SafetyPathCallbackExceptionIsContainedThroughTheSinkInsteadOfTerminating) {
  std::mutex reasons_mutex;
  std::vector<std::string> reasons;
  std::atomic<bool> stop_followup_fired{false};
  std::atomic<bool> background_followup_fired{false};
  SimpleScheduler scheduler;
  scheduler.set_safety_path_fault_sink([&](const std::string_view reason) {
    std::lock_guard<std::mutex> lock(reasons_mutex);
    reasons.emplace_back(reason);
  });

  const auto now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  scheduler.schedule(
      now_ns, []() { throw std::runtime_error("stop stage failed"); },
      SchedulerPriority::StopControl);
  scheduler.schedule(
      now_ns + 1'000'000LL, [&]() { stop_followup_fired.store(true); },
      SchedulerPriority::StopControl);
  scheduler.schedule(
      now_ns, []() { throw 1; }, SchedulerPriority::RuntimeSafety);
  scheduler.schedule(
      now_ns, []() { throw std::runtime_error("diagnostic failed"); },
      SchedulerPriority::Background);
  scheduler.schedule(
      now_ns + 1'000'000LL, [&]() { background_followup_fired.store(true); },
      SchedulerPriority::Background);

  const auto contained_both = [&]() {
    std::lock_guard<std::mutex> lock(reasons_mutex);
    return reasons.size() >= 2U;
  };
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while ((!contained_both() || !stop_followup_fired.load() ||
          !background_followup_fired.load()) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(stop_followup_fired.load())
      << "StopControl lane stopped after a contained callback exception";
  EXPECT_TRUE(background_followup_fired.load());
  {
    std::lock_guard<std::mutex> lock(reasons_mutex);
    std::sort(reasons.begin(), reasons.end());
    EXPECT_EQ(reasons, (std::vector<std::string>{
                           "scheduler_runtime_safety_callback_exception",
                           "scheduler_stop_control_callback_exception"}));
  }
  EXPECT_TRUE(scheduler.drain_all_for(std::chrono::seconds(1)));
  scheduler.set_safety_path_fault_sink({});
}

TEST(NativeSchedulerTest, DrainWaitsForDequeuedRuntimeSafetyCallback) {
  SimpleScheduler scheduler;
  std::promise<void> callback_entered;
  auto callback_entered_future = callback_entered.get_future();
  std::promise<void> release_callback;
  auto release_callback_future = release_callback.get_future();

  const auto now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  scheduler.schedule(
      now_ns,
      [&]() {
        callback_entered.set_value();
        release_callback_future.wait();
      },
      SchedulerPriority::RuntimeSafety);

  ASSERT_EQ(callback_entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  auto drain_future = std::async(std::launch::async, [&]() {
    scheduler.drain(SchedulerPriority::RuntimeSafety);
  });
  EXPECT_EQ(drain_future.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);

  release_callback.set_value();
  EXPECT_EQ(drain_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
}

TEST(NativeSchedulerTest, DrainForTimesOutWhileRuntimeSafetyCallbackIsBlocked) {
  SimpleScheduler scheduler;
  std::promise<void> callback_entered;
  auto callback_entered_future = callback_entered.get_future();
  std::promise<void> release_callback;
  auto release_callback_future = release_callback.get_future();

  const auto now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  scheduler.schedule(
      now_ns,
      [&]() {
        callback_entered.set_value();
        release_callback_future.wait();
      },
      SchedulerPriority::RuntimeSafety);

  ASSERT_EQ(callback_entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(
      scheduler.drain_for(SchedulerPriority::RuntimeSafety,
                          std::chrono::milliseconds(20)));
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count(),
            250);

  release_callback.set_value();
  EXPECT_TRUE(
      scheduler.drain_for(SchedulerPriority::RuntimeSafety,
                          std::chrono::seconds(1)));
}

TEST(NativeSchedulerTest, DrainAllSharesOneDeadlineAcrossBlockedLanes) {
  SimpleScheduler scheduler;
  std::promise<void> release;
  const auto gate = release.get_future().share();
  std::promise<void> first_entered;
  std::promise<void> second_entered;
  auto first_ready = first_entered.get_future();
  auto second_ready = second_entered.get_future();
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  scheduler.schedule(now_ns, [&] { first_entered.set_value(); gate.wait(); },
                     SchedulerPriority::RuntimeSafety);
  scheduler.schedule(now_ns, [&] { second_entered.set_value(); gate.wait(); },
                     SchedulerPriority::StopControl);
  const bool first_started = first_ready.wait_for(std::chrono::seconds(2)) ==
                             std::future_status::ready;
  const bool second_started = second_ready.wait_for(std::chrono::seconds(2)) ==
                              std::future_status::ready;
  auto drain = std::async(std::launch::async, [&] {
    return scheduler.drain_all_for(std::chrono::milliseconds(250));
  });
  // Two blocked lanes used to consume a full timeout each. Always release
  // before assertions so a failed timing check cannot strand worker threads.
  const bool completed_in_shared_budget =
      drain.wait_for(std::chrono::milliseconds(450)) == std::future_status::ready;
  release.set_value();
  const bool drained = drain.get();
  EXPECT_TRUE(first_started);
  EXPECT_TRUE(second_started);
  EXPECT_TRUE(completed_in_shared_budget);
  EXPECT_FALSE(drained);
  EXPECT_TRUE(scheduler.drain_all_for(std::chrono::seconds(1)));
}

TEST(WriterPreferringSharedGateTest, WaitingWriterBlocksNewReadersAndIsBounded) {
  WriterPreferringSharedGate gate;
  // A reader is in flight (e.g. a host frame query on the current model).
  gate.lock_shared();
  ASSERT_TRUE(gate.try_lock_shared());  // readers share while no writer waits
  gate.unlock_shared();

  std::atomic<bool> replaced{false};
  std::thread writer([&] {
    gate.lock();
    replaced.store(true);
    gate.unlock();
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!gate.writer_claimed()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::yield();
  }
  // Once the writer waits, a new reader cannot overlap the in-flight one and
  // extend the writer's wait; the writer waits only for the reader it found.
  EXPECT_FALSE(gate.try_lock_shared());
  EXPECT_FALSE(replaced.load());
  gate.unlock_shared();
  writer.join();
  EXPECT_TRUE(replaced.load());
  EXPECT_FALSE(gate.writer_claimed());
  ASSERT_TRUE(gate.try_lock_shared());
  gate.unlock_shared();
}

TEST(WriterPreferringSharedGateTest, BlockedReaderProceedsAfterTheWriter) {
  WriterPreferringSharedGate gate;
  gate.lock();
  std::atomic<bool> read{false};
  std::thread reader([&] {
    gate.lock_shared();
    read.store(true);
    gate.unlock_shared();
  });
  EXPECT_FALSE(gate.try_lock_shared());
  EXPECT_FALSE(read.load());
  gate.unlock();
  reader.join();
  EXPECT_TRUE(read.load());
}

namespace {

[[nodiscard]] ProjectionRevisionDTO PublishedProjection(
    const std::uint64_t scene_revision_id) {
  return {
      RetainedBundleId("bundle.active"),
      GraphRevision("graph.active"),
      SceneRevision{
          SceneId("sha256:1111111111111111111111111111111111111111111111111111111111111111"),
          scene_revision_id,
          ContentDigest("sha256:2222222222222222222222222222222222222222222222222222222222222222"),
      },
  };
}

}  // namespace

TEST(RetainedViewPredecessor,
     SceneRevisionAdvancedByRunAttachDetachStillFollowsPredecessor) {
  // Published at Scene revision 5; a pick-and-place cycle's attach, detach
  // and reset advanced the live Scene to revision 8 before Revert.
  const auto published = PublishedProjection(5U);
  const auto predecessor = PublishedProjection(8U);
  const RetainedBundleId bundle("bundle.active");

  EXPECT_TRUE(RetainedViewFollowsPredecessor(
      bundle, published, bundle, predecessor));
}

TEST(RetainedViewPredecessor, AnotherDeploymentCrossesThePredecessor) {
  const auto published = PublishedProjection(5U);
  const RetainedBundleId bundle("bundle.active");
  auto other_graph = published;
  other_graph.graph_revision = GraphRevision("graph.other");
  auto other_bundle = published;
  other_bundle.retained_bundle_id = RetainedBundleId("bundle.other");

  EXPECT_FALSE(RetainedViewFollowsPredecessor(
      bundle, published, bundle, other_graph));
  EXPECT_FALSE(RetainedViewFollowsPredecessor(
      bundle, published, bundle, other_bundle));
  EXPECT_FALSE(RetainedViewFollowsPredecessor(
      bundle, published, RetainedBundleId("bundle.other"), published));
}

TEST(ExecutionMotionTargetLimitsTest,
     SharedRuntimeRequiresBothRobotsAndExcludesForeignUnit) {
  const RuntimeUnitId shared("runtime.shared");
  const ControllableComponentId first("robot.first");
  const ControllableComponentId second("robot.second");
  const CommandTargetId arm("arm");
  const ControlBindingDTO first_binding{
      ControlBindingId("binding.first"), ControlBindingRevision("binding-revision"),
      first, arm, CapabilityId("execution.trajectory.execute"),
      CapabilityDefinitionRevision("capability-revision"), ControlDomainId("domain"),
      {}, ControlInterfaceContractId("interface"), ControlInterfaceRevision("interface-revision"),
      ControllerTopologyId("topology"), TopologyRevision("topology-revision"),
      {ControlRouteId("route")}, {}, ProviderInstanceId("provider"), shared,
      ProviderManifestId("manifest"), ProviderManifestRevision("manifest-revision"),
      StateAuthorityId("state"), StateAuthorityClass::kCommanded,
      StopEvidencePolicyId("stop"), StopEvidencePolicyRevision("stop-revision"),
      ContainmentGranularity::kResource, {}};
  auto second_binding = first_binding;
  second_binding.controllable_component_id = second;
  auto foreign_binding = first_binding;
  foreign_binding.controllable_component_id = ControllableComponentId("robot.foreign");
  foreign_binding.provider_runtime_unit_id = RuntimeUnitId("runtime.foreign");
  const std::vector<ControlBindingDTO> bindings{
      first_binding, second_binding, foreign_binding};
  const std::vector<ExecutionMotionTargetKey> both{{first, arm}, {second, arm}};
  for (const auto &selected : {first, second}) {
    EXPECT_TRUE(execution_motion_target_limit_keys_match(
        bindings, both, shared, selected, true));
    EXPECT_FALSE(execution_motion_target_limit_keys_match(
        bindings, std::vector<ExecutionMotionTargetKey>{{selected, arm}},
        shared, selected, true));
  }
  auto unknown = both;
  unknown.emplace_back(foreign_binding.controllable_component_id, arm);
  EXPECT_FALSE(execution_motion_target_limit_keys_match(
      bindings, unknown, shared, first, true));
  auto duplicate = both;
  duplicate.insert(duplicate.begin(), both.front());
  EXPECT_FALSE(execution_motion_target_limit_keys_match(
      bindings, duplicate, shared, first, true));
  EXPECT_FALSE(execution_motion_target_limit_keys_match(
      bindings, std::vector<ExecutionMotionTargetKey>{{first, CommandTargetId("unknown")},
                                                     {second, arm}},
      shared, first, true));
  // Native configuration remains an exact selected-component slice.
  for (const auto &selected : {first, second}) {
    EXPECT_TRUE(execution_motion_target_limit_keys_match(
        bindings, std::vector<ExecutionMotionTargetKey>{{selected, arm}},
        shared, selected, false));
    EXPECT_FALSE(execution_motion_target_limit_keys_match(
        bindings, both, shared, selected, false));
  }
}
