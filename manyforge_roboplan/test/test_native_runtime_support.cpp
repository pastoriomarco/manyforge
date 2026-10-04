#include <gtest/gtest.h>

#include <manyforge_roboplan/native_runtime_support.hpp>
#include <manyforge_roboplan/retained_native_view_predecessor.hpp>
#include <manyforge_roboplan/writer_preferring_shared_gate.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sched.h>
#endif

using namespace manyforge::roboplan_backend;
using namespace manyforge::core;

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

  // Monitoring callback must fire even while Safety is parked.
  scheduler.schedule(
      schedule_at(20'000'000LL),
      [&]() { monitoring_fired.store(true); },
      SchedulerPriority::Monitoring);

  // Wait up to 500 ms for the watchdog to fire under contention.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!monitoring_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(runtime_safety_running.load())
      << "Runtime-safety callback did not reach its parking loop";
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

  scheduler.schedule(
      schedule_at(20'000'000LL),
      [&]() { controller_fired.store(true); },
      SchedulerPriority::ControllerCommand);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!controller_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_EQ(blocked_roles_running.load(), 5);
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
  scheduler.schedule(
      schedule_at(20'000'000LL),
      [&]() { runtime_safety_fired.store(true); },
      SchedulerPriority::RuntimeSafety);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!runtime_safety_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(lifecycle_running.load());
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

  scheduler.schedule(
      schedule_at(20'000'000LL),
      [&]() { stop_control_fired.store(true); },
      SchedulerPriority::StopControl);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!stop_control_fired.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_EQ(blocked_roles_running.load(), 3);
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
      {"MANYFORGE_RT_STOP_CONTROL_PRIORITY", "35"},
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
  EXPECT_EQ(stop.priority, 35);
  EXPECT_EQ(stop.cpu_affinity, controller.cpu_affinity)
      << "StopControl deliberately shares the controller-command reservation "
         "unless deployment selects a separate CPU";
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
      {"MANYFORGE_RT_STOP_CONTROL_PRIORITY", "35"},
  };
  EXPECT_THROW(
      (void)SimpleSchedulerConfig::from_environment(environment),
      std::invalid_argument);
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

TEST(RetainedNativeViewPredecessor,
     SceneRevisionAdvancedByRunAttachDetachStillFollowsPredecessor) {
  // Published at Scene revision 5; a pick-and-place cycle's attach, detach
  // and reset advanced the live Scene to revision 8 before Revert.
  const auto published = PublishedProjection(5U);
  const auto predecessor = PublishedProjection(8U);
  const RetainedBundleId bundle("bundle.active");

  EXPECT_TRUE(RetainedNativeViewFollowsPredecessor(
      bundle, published, bundle, predecessor));
}

TEST(RetainedNativeViewPredecessor, AnotherDeploymentCrossesThePredecessor) {
  const auto published = PublishedProjection(5U);
  const RetainedBundleId bundle("bundle.active");
  auto other_graph = published;
  other_graph.graph_revision = GraphRevision("graph.other");
  auto other_bundle = published;
  other_bundle.retained_bundle_id = RetainedBundleId("bundle.other");

  EXPECT_FALSE(RetainedNativeViewFollowsPredecessor(
      bundle, published, bundle, other_graph));
  EXPECT_FALSE(RetainedNativeViewFollowsPredecessor(
      bundle, published, bundle, other_bundle));
  EXPECT_FALSE(RetainedNativeViewFollowsPredecessor(
      bundle, published, RetainedBundleId("bundle.other"), published));
}
