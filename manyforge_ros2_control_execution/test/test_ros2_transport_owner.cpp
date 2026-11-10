// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0

#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <manyforge_ros2_control_execution/graph_event_observer.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include "../src/scene_projection_worker.hpp"

#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <control_msgs/msg/joint_trajectory_controller_state.hpp>
#include <control_msgs/srv/query_trajectory_state.hpp>
#include <gtest/gtest.h>
#include <rclcpp_action/rclcpp_action.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::ros2_control_execution {



TEST(Ros2TransportOwner, CachedContextTracksPrivateShutdown) {
  auto resources_owner = Ros2TransportResources::create("cached_context_test", "", 1);
  auto &resources = *resources_owner;
  EXPECT_TRUE(resources.cached_context_valid());
  resources.quiesce();
  EXPECT_FALSE(resources.cached_context_valid());
}

TEST(Ros2TransportOwner, GraphObserverRefreshesServiceOnAppearanceAndRemoval) {
  using Service = control_msgs::srv::QueryTrajectoryState;
  auto resources_owner = Ros2TransportResources::create("graph_observer_test", "", 1);
  auto &resources = *resources_owner;
  resources.start();
  auto ready = std::make_shared<std::atomic<int>>(-1);
  auto samples = std::make_shared<std::atomic<unsigned>>(0U);
  rclcpp::Client<Service>::SharedPtr client;
  std::shared_ptr<GraphEventObserver> observer;
  resources.invoke_creation([&] {
    auto node = resources.node();
    auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    client = resources.retain_entity(node->create_client<Service>("/manyforge_graph_observer_test"));
    std::weak_ptr<rclcpp::Client<Service>> weak = client;
    auto refresh = [weak, ready, samples] {
      if (const auto current = weak.lock()) {
        ready->store(current->service_is_ready() ? 1 : 0);
        ++*samples;
      }
    };
    observer = resources.retain_entity(std::make_shared<GraphEventObserver>(node, group, refresh));
    refresh();
  });
  EXPECT_EQ(ready->load(), 0);
  rclcpp::NodeOptions options;
  options.context(resources.node()->get_node_base_interface()->get_context());
  auto server = std::make_shared<rclcpp::Node>("graph_observer_server", options);
  auto service = server->create_service<Service>(
      "/manyforge_graph_observer_test",
      [](const std::shared_ptr<Service::Request>, std::shared_ptr<Service::Response>) {});
  const auto wait_for = [&](int expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (ready->load() != expected && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return ready->load() == expected;
  };
  EXPECT_TRUE(wait_for(1));
  const auto before_request = samples->load();
  EXPECT_TRUE(observer->request_refresh());
  const auto sample_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (samples->load() == before_request && std::chrono::steady_clock::now() < sample_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_GT(samples->load(), before_request);
  service.reset();
  server.reset();
  EXPECT_TRUE(wait_for(0));
  observer.reset();
  client.reset();
  resources.quiesce();
}
class Ros2TransportOwnerTestPeer {
public:
  static bool dispatch_starting(Ros2TransportResources &resources) {
    std::lock_guard<std::mutex> lock(resources.dispatch_mutex_);
    return resources.dispatch_starting_;
  }
  static bool quiesce_started(Ros2TransportResources &resources) {
    std::lock_guard lock(resources.lifecycle_mutex_);
    return resources.quiesced_;
  }
  static void set_wait_bound(Ros2TransportResources &resources,
                             std::chrono::nanoseconds bound) {
    resources.owner_wait_bound_ = bound;
  }
  static void fail_trigger(Ros2TransportResources &resources) {
    *resources.dispatch_trigger_ = [] { throw std::runtime_error("test notifier failure"); };
  }
  static void set_trigger(Ros2TransportResources &resources, std::function<void()> trigger) {
    *resources.dispatch_trigger_ = std::move(trigger);
  }
  static bool notification_idle(Ros2TransportResources &resources) {
    std::lock_guard<std::mutex> lock(resources.dispatch_mutex_);
    return resources.active_dispatches_ == 0 && !resources.owner_notification_active_;
  }
  static void hold_foreign_invocation(Ros2TransportResources &resources) {
    std::lock_guard<std::mutex> lock(resources.dispatch_mutex_);
    ++resources.foreign_invocations_;
  }
  static void release_foreign_invocation(Ros2TransportResources &resources) {
    {
      std::lock_guard<std::mutex> lock(resources.dispatch_mutex_);
      --resources.foreign_invocations_;
    }
    resources.dispatch_cv_.notify_all();
  }
  static std::thread::id time_critical_thread_id(Ros2TransportResources &resources) {
    std::lock_guard<std::mutex> lock(resources.dispatch_mutex_);
    return resources.time_critical_thread_id_;
  }
  static bool time_critical_joined(Ros2TransportResources &resources) {
    return !resources.time_critical_thread_.joinable() &&
           resources.time_critical_spin_finished_.load();
  }
  static bool owner_joined(Ros2TransportResources &resources) {
    return !resources.spin_thread_.joinable() && resources.spin_finished_.load();
  }
  static bool wait_owner_exit(Ros2TransportResources &resources) {
    using namespace std::chrono_literals;
    std::unique_lock<std::mutex> lock(resources.dispatch_mutex_);
    return resources.dispatch_cv_.wait_for(lock, 3s, [&] { return resources.owner_exited_; });
  }
  static bool wait_stopping(Ros2TransportResources &resources) {
    using namespace std::chrono_literals;
    std::unique_lock<std::mutex> lock(resources.dispatch_mutex_);
    return resources.dispatch_cv_.wait_for(lock, 3s, [&] { return resources.dispatch_stopping_; });
  }
  static bool wait_ordinary_slot(Ros2TransportResources &resources) {
    using namespace std::chrono_literals;
    std::unique_lock<std::mutex> lock(resources.dispatch_mutex_);
    return resources.dispatch_cv_.wait_for(lock, 3s, [&] { return resources.ordinary_slot_ != nullptr; });
  }
  static void pump_once(Ros2TransportResources &resources) {
    resources.executor_->spin_once(std::chrono::nanoseconds::zero());
  }
  static void pump_one_task(Ros2TransportResources &resources, int &task_calls,
                            const int *callback_calls = nullptr) {
    Ros2TransportResources::DispatchTask task{
        [&] { ++task_calls; }, false, false, {}};
    {
      std::lock_guard<std::mutex> lock(resources.dispatch_mutex_);
      resources.ordinary_slot_ = &task;
    }
    try {
      // Trigger before pumping, as an invocation can do before the first spin
      // or while entity changes are waiting to be recollected.
      resources.dispatch_guard_->trigger();
      for (int step = 0; step < 1000 && (!task.done || (callback_calls &&
                                                        *callback_calls == 0));
           ++step)
        pump_once(resources);
      // Extra nonblocking pumps check that the same task does not run twice.
      for (int step = 0; step < 10; ++step)
        pump_once(resources);
    } catch (...) {
      std::lock_guard<std::mutex> lock(resources.dispatch_mutex_);
      resources.ordinary_slot_ = nullptr;
      throw;
    }
    {
      std::lock_guard<std::mutex> lock(resources.dispatch_mutex_);
      resources.ordinary_slot_ = nullptr;
    }
    EXPECT_TRUE(task.done);
    EXPECT_TRUE(task.ran);
  }
  static std::vector<int> drive_occupied_slots(Ros2TransportResources &resources) {
    using namespace std::chrono_literals;
    std::vector<int> order;
    Ros2TransportResources::DispatchTask ordinary{[&] { order.push_back(1); }, false, false, {}};
    Ros2TransportResources::DispatchTask close{[&] { order.push_back(2); }, false, false, {}};
    {
      std::lock_guard<std::mutex> lock(resources.dispatch_mutex_);
      resources.ordinary_slot_ = &ordinary;
      resources.close_slot_ = &close;
    }
    try {
      resources.dispatch_guard_->trigger();
      // Other ready ROS entities may be selected first. Each step is nonblocking.
      for (int step = 0; step < 100 && order.size() < 2; ++step)
        resources.executor_->spin_once(0ns);
    } catch (...) {
      std::lock_guard<std::mutex> lock(resources.dispatch_mutex_);
      resources.ordinary_slot_ = nullptr;
      resources.close_slot_ = nullptr;
      throw;
    }
    {
      std::lock_guard<std::mutex> lock(resources.dispatch_mutex_);
      resources.ordinary_slot_ = nullptr;
      resources.close_slot_ = nullptr;
    }
    return order;
  }
};

TEST(Ros2TransportOwner, QuiesceWinsStartBlockedByPrestartInvocation) {
  using namespace std::chrono_literals;
  auto owner = Ros2TransportResources::create("owner_start_close_race", "", 1);
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  auto invocation = std::async(std::launch::async, [&] {
    owner->invoke([&] { entered.set_value(); release_future.wait(); });
  });
  const bool active = entered_future.wait_for(2s) == std::future_status::ready;
  auto starting = std::async(std::launch::async, [&]() -> std::string {
    try { owner->start(); return "started"; }
    catch (const core::TypedError& error) { return error.code(); }
    catch (const std::exception& error) { return error.what(); }
  });
  const auto until = std::chrono::steady_clock::now() + 2s;
  while (!Ros2TransportOwnerTestPeer::dispatch_starting(*owner) &&
         std::chrono::steady_clock::now() < until) std::this_thread::yield();
  const bool saw_start = Ros2TransportOwnerTestPeer::dispatch_starting(*owner);
  const bool first = owner->quiesce_until(std::chrono::steady_clock::now() + 20ms);
  const bool retained = owner->cached_context_valid() && !owner->joined();
  release.set_value();
  invocation.get();
  const auto start_result = starting.get();
  const bool second = owner->quiesce_until(std::chrono::steady_clock::now() + 2s);
  EXPECT_TRUE(active);
  EXPECT_TRUE(saw_start);
  EXPECT_FALSE(first);
  EXPECT_TRUE(retained);
  EXPECT_EQ(start_result, "component_not_ready");
  EXPECT_TRUE(second);
}

TEST(Ros2TransportOwner, BoundedQuiesceDoesNotWaitForStuckSdkSend) {
  using namespace std::chrono_literals;
  auto owner = Ros2TransportResources::create("owner_bounded_sdk", "", 1);
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  owner->start();
  auto send = std::async(std::launch::async, [&] {
    return owner->try_sdk_send([&] { entered.set_value(); release_future.wait(); });
  });
  const bool started = entered_future.wait_for(2s) == std::future_status::ready;
  const bool first = started && owner->quiesce_until(std::chrono::steady_clock::now() + 20ms);
  const bool retained = owner->cached_context_valid() && !owner->joined();
  const bool admitted_after_close = owner->try_sdk_send([] {});
  release.set_value();
  const bool sent = send.get();
  const bool second = owner->quiesce_until(std::chrono::steady_clock::now() + 2s);
  EXPECT_TRUE(started);
  EXPECT_FALSE(first);
  EXPECT_TRUE(retained);
  EXPECT_FALSE(admitted_after_close);
  EXPECT_TRUE(sent);
  EXPECT_TRUE(second);
}

TEST(Ros2TransportOwner, BoundedQuiesceRetainsContextUntilOwnerExits) {
  using namespace std::chrono_literals;
  auto owner = Ros2TransportResources::create("owner_bounded_quiesce", "", 1);
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  owner->set_owner_notification([&] { entered.set_value(); release_future.wait(); });
  owner->start();
  const bool notified = owner->notify_owner();
  const bool started = entered_future.wait_for(2s) == std::future_status::ready;
  const bool first = started && owner->quiesce_until(std::chrono::steady_clock::now() + 20ms);
  const bool retained = owner->cached_context_valid() && !owner->joined();
  release.set_value();
  const bool second = owner->quiesce_until(std::chrono::steady_clock::now() + 2s);
  EXPECT_TRUE(notified);
  EXPECT_TRUE(started);
  EXPECT_FALSE(first);
  EXPECT_TRUE(retained);
  EXPECT_TRUE(second);
  EXPECT_TRUE(owner->joined());
}

TEST(Ros2TransportOwner, BoundedQuiesceWaitsForSpinExitAfterFaultObserver) {
  using namespace std::chrono_literals;
  auto owner = Ros2TransportResources::create("owner_bounded_fault_exit", "", 1);
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  owner->set_owner_notification([] { throw std::runtime_error("test owner fault"); });
  owner->set_executor_fault_observer([&](std::string_view) {
    entered.set_value();
    release_future.wait();
  });
  owner->start();
  const bool notified = owner->notify_owner();
  const bool started = entered_future.wait_for(2s) == std::future_status::ready;
  const bool first = started && owner->quiesce_until(std::chrono::steady_clock::now() + 20ms);
  const bool retained = owner->cached_context_valid() && !owner->joined();
  release.set_value();
  const bool second = owner->quiesce_until(std::chrono::steady_clock::now() + 2s);
  EXPECT_TRUE(notified);
  EXPECT_TRUE(started);
  EXPECT_FALSE(first);
  EXPECT_TRUE(retained);
  EXPECT_TRUE(second);
  EXPECT_TRUE(owner->joined());
}

TEST(Ros2TransportOwner, WinningQuiescePinsOwnerUntilJoinCompletes) {
  auto owner = Ros2TransportResources::create("owner_quiesce_pin", "", 1);
  std::weak_ptr<Ros2TransportResources> weak = owner;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  owner->set_owner_notification([&] { entered.set_value(); release_future.wait(); });
  owner->start();
  ASSERT_TRUE(owner->notify_owner());
  ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  auto *const raw = owner.get();
  auto first = std::async(std::launch::async, [raw] { raw->quiesce(); });
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!Ros2TransportOwnerTestPeer::quiesce_started(*owner) &&
         std::chrono::steady_clock::now() < end)
    std::this_thread::yield();
  const bool started = Ros2TransportOwnerTestPeer::quiesce_started(*owner);
  bool held = false;
  if (started) {
    owner->quiesce(); // second caller may return while the first waits for join
    owner.reset();
    held = !weak.expired();
  }
  release.set_value();
  first.get();
  owner.reset();
  EXPECT_TRUE(started);
  EXPECT_TRUE(held);
  EXPECT_TRUE(weak.expired());
}

#ifndef NDEBUG
TEST(Ros2TransportOwner, DebugRejectsFinalReleaseOnOwnerThread) {
  EXPECT_DEATH({
    auto owner = Ros2TransportResources::create("owner_final_release_assert", "", 1);
    owner->set_owner_notification([&] { owner.reset(); });
    owner->start();
    (void)owner->notify_owner();
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }, "on_owner_thread");
}
#endif
namespace {

struct RetirementProbe {
  explicit RetirementProbe(std::promise<std::thread::id> &destroyed)
      : destroyed(destroyed) {}
  ~RetirementProbe() { destroyed.set_value(std::this_thread::get_id()); }
  std::promise<std::thread::id> &destroyed;
};

struct NestedRetirementProbe {
  explicit NestedRetirementProbe(std::shared_ptr<RetirementProbe> nested)
      : nested(std::move(nested)) {}
  std::shared_ptr<RetirementProbe> nested;
};

class ProjectionSink final : public core::IExecutionStateSink {
public:
  core::ExecutionStateSinkResult apply_group_joint_positions(
      const std::string &, const std::vector<double> &,
      core::ExecutionStateUpdateMode) override {
    ++calls;
    return {core::ExecutionStateSinkStatus::kApplied};
  }
  std::optional<double> group_joint_position(const std::string &,
                                              const std::string &) const override {
    return std::nullopt;
  }
  core::ExecutionStateSinkResult apply_group_joint_position(
      const std::string &, const std::string &, double,
      core::ExecutionStateUpdateMode) override {
    ++calls;
    return {core::ExecutionStateSinkStatus::kApplied};
  }
  std::atomic<int> calls{0};
};

TEST(Ros2TransportOwner, ExpiredDeadlineAcceptsAlreadyJoinedOwnedProjection) {
  using namespace std::chrono_literals;
  core::FakeClock clock;
  auto sink = std::make_shared<ProjectionSink>();
  auto owner = Ros2TransportResources::create("joined_projection_expired", "", 1, sink, "arm", clock);
  ASSERT_TRUE(owner->projection_worker().stop_and_join_for(2s));
  EXPECT_TRUE(owner->quiesce_until(std::chrono::steady_clock::now() - 1ms));
  EXPECT_TRUE(owner->joined());
}

TEST(Ros2TransportOwner, BoundedQuiesceRetainsContextForBlockedOwnedProjection) {
  using namespace std::chrono_literals;
  struct BlockingSink final : core::IExecutionStateSink {
    std::promise<void> entered;
    std::shared_future<void> release;
    explicit BlockingSink(std::shared_future<void> value) : release(std::move(value)) {}
    core::ExecutionStateSinkResult apply_group_joint_positions(
        const std::string&, const std::vector<double>&, core::ExecutionStateUpdateMode) override {
      entered.set_value();
      release.wait();
      return {core::ExecutionStateSinkStatus::kApplied};
    }
    std::optional<double> group_joint_position(const std::string&, const std::string&) const override {
      return std::nullopt;
    }
    core::ExecutionStateSinkResult apply_group_joint_position(
        const std::string&, const std::string&, double, core::ExecutionStateUpdateMode) override {
      return {core::ExecutionStateSinkStatus::kApplied};
    }
  };
  core::FakeClock clock;
  std::promise<void> release;
  auto sink = std::make_shared<BlockingSink>(release.get_future().share());
  auto entered = sink->entered.get_future();
  auto owner = Ros2TransportResources::create("bounded_owned_projection", "", 1, sink, "arm", clock);
  const bool submitted = owner->projection_worker().submit_terminal({0.5}, 0);
  const bool active = entered.wait_for(2s) == std::future_status::ready;
  const bool first = active && owner->quiesce_until(std::chrono::steady_clock::now() + 20ms);
  const bool zero_budget = owner->quiesce_until(std::chrono::steady_clock::now() - 1ms);
  const bool retained = owner->cached_context_valid() && !owner->joined();
  release.set_value();
  const bool second = owner->quiesce_until(std::chrono::steady_clock::now() + 2s);
  EXPECT_TRUE(submitted);
  EXPECT_TRUE(active);
  EXPECT_FALSE(first);
  EXPECT_FALSE(zero_budget);
  EXPECT_TRUE(retained);
  EXPECT_TRUE(second);
}

TEST(Ros2TransportOwner, TakenProjectionWorkerSurvivesResourceShutdown) {
  using namespace std::chrono_literals;
  core::FakeClock clock;
  auto sink = std::make_shared<ProjectionSink>();
  auto resources = Ros2TransportResources::create(
      "projection_transfer", "", 1, sink, "arm", clock);
  auto *original = &resources->projection_worker();
  auto worker = resources->take_projection_worker();
  EXPECT_EQ(worker.get(), original);
  EXPECT_THROW(resources->take_projection_worker(), std::logic_error);
  EXPECT_THROW(resources->projection_worker(), std::logic_error);
  resources->start();
  resources->quiesce();
  resources.reset();
  EXPECT_TRUE(worker->submit_terminal({0.5}, 0));
  EXPECT_TRUE(worker->drain_for(3s));
  EXPECT_EQ(sink->calls.load(), 1);
  EXPECT_TRUE(worker->stop_and_join_for(3s));
}

TEST(Ros2TransportOwner, ProjectionTransferIsPreStartAndOptional) {
  using namespace std::chrono_literals;
  core::FakeClock clock;
  auto sink = std::make_shared<ProjectionSink>();
  auto no_worker_owner = Ros2TransportResources::create("projection_absent", "", 1);
  auto &no_worker = *no_worker_owner;
  EXPECT_THROW(no_worker.take_projection_worker(), std::logic_error);
  auto resources_owner = Ros2TransportResources::create("projection_not_taken", "", 1, sink, "arm", clock);
  auto &resources = *resources_owner;
  auto &worker = resources.projection_worker();
  resources.start();
  EXPECT_THROW(resources.take_projection_worker(), std::logic_error);
  resources.quiesce();
  EXPECT_THROW(resources.take_projection_worker(), std::logic_error);
  EXPECT_FALSE(worker.submit_terminal({0.5}, 0));
  EXPECT_TRUE(worker.stop_and_join_for(3s));
}

TEST(Ros2TransportOwner, SdkSendGateClosesOnFaultBeforeObserver) {
  using namespace std::chrono_literals;
  auto resources_owner = Ros2TransportResources::create("sdk_send_fault", "", 1);
  auto &resources = *resources_owner;
  auto token = resources.executor_failure_token();
  std::atomic<bool> woken{false};
  std::stop_callback wake(token, [&] {
    // A failure token must not be signaled with dispatch/send locks held.
    const bool owner_thread = resources.on_owner_thread();
    const bool admitted = resources.try_sdk_send([] {});
    woken.store(owner_thread && !admitted);
  });
  std::promise<bool> observer_result;
  auto result = observer_result.get_future();
  resources.set_owner_notification([] { throw std::runtime_error("send gate fault"); });
  resources.set_executor_fault_observer([&](std::string_view) {
    bool sent = false;
    const bool admitted = resources.try_sdk_send([&] { sent = true; });
    observer_result.set_value(resources.executor_failure_started() && woken.load() &&
                              token.stop_requested() && !admitted && !sent);
  });
  int calls = 0;
  EXPECT_TRUE(resources.try_sdk_send([&] { ++calls; }));
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(resources.executor_failure_started());
  EXPECT_FALSE(resources.joined());
  resources.start();
  ASSERT_TRUE(resources.notify_owner());
  ASSERT_EQ(result.wait_for(3s), std::future_status::ready);
  EXPECT_TRUE(result.get());
  resources.quiesce();
  EXPECT_TRUE(resources.joined());
  EXPECT_FALSE(resources.try_sdk_send([&] { ++calls; }));
  EXPECT_EQ(calls, 1);
}

TEST(Ros2TransportOwner, NotifierFailureSignalsTokenBeforeObserver) {
  using namespace std::chrono_literals;
  auto resources_owner = Ros2TransportResources::create("notifier_failure_token", "", 1);
  auto &resources = *resources_owner;
  auto token = resources.executor_failure_token();
  std::promise<bool> observed;
  auto result = observed.get_future();
  resources.set_owner_notification([] {});
  resources.set_executor_fault_observer([&](std::string_view) {
    observed.set_value(token.stop_requested());
  });
  Ros2TransportOwnerTestPeer::fail_trigger(resources);
  resources.start();
  EXPECT_FALSE(resources.notify_owner());
  ASSERT_EQ(result.wait_for(3s), std::future_status::ready);
  EXPECT_TRUE(result.get());
  resources.quiesce();
}

TEST(Ros2TransportOwner, CleanQuiesceDoesNotSignalFailureToken) {
  auto resources_owner = Ros2TransportResources::create("clean_failure_token", "", 1);
  auto &resources = *resources_owner;
  auto token = resources.executor_failure_token();
  resources.start();
  resources.quiesce();
  EXPECT_FALSE(token.stop_requested());
}

TEST(Ros2TransportOwner, SdkSendAndCloseSerializeAtSendBoundary) {
  using namespace std::chrono_literals;
  auto resources_owner = Ros2TransportResources::create("sdk_send_close", "", 1);
  auto &resources = *resources_owner;
  resources.start();
  std::promise<void> entered;
  auto has_entered = entered.get_future();
  std::promise<void> release;
  auto allow_return = release.get_future().share();
  auto sending = std::async(std::launch::async, [&] {
    return resources.try_sdk_send([&] {
      entered.set_value();
      allow_return.wait();
    });
  });
  const auto observed = has_entered.wait_for(3s);
  auto closing = std::async(std::launch::async, [&] { resources.quiesce(); });
  const auto before_release = closing.wait_for(0ms);
  release.set_value();
  const bool sent = sending.get();
  closing.get();
  EXPECT_EQ(observed, std::future_status::ready);
  EXPECT_EQ(before_release, std::future_status::timeout);
  EXPECT_TRUE(sent);
  EXPECT_TRUE(resources.joined());
  bool later = false;
  EXPECT_FALSE(resources.try_sdk_send([&] { later = true; }));
  EXPECT_FALSE(later);
}

TEST(Ros2TransportOwner, HealthySdkSendsCanOverlap) {
  using namespace std::chrono_literals;
  auto resources_owner = Ros2TransportResources::create("sdk_send_parallel", "", 1);
  auto &resources = *resources_owner;
  std::promise<void> first_entered, second_entered, release;
  auto first_ready = first_entered.get_future();
  auto second_ready = second_entered.get_future();
  auto allow_return = release.get_future().share();
  auto first = std::async(std::launch::async, [&] {
    return resources.try_sdk_send([&] { first_entered.set_value(); allow_return.wait(); });
  });
  const auto first_status = first_ready.wait_for(3s);
  auto second = std::async(std::launch::async, [&] {
    return resources.try_sdk_send([&] { second_entered.set_value(); allow_return.wait(); });
  });
  const auto second_status = second_ready.wait_for(3s);
  release.set_value();
  EXPECT_TRUE(first.get());
  EXPECT_TRUE(second.get());
  EXPECT_EQ(first_status, std::future_status::ready);
  EXPECT_EQ(second_status, std::future_status::ready);
}

TEST(Ros2TransportOwner, EntityBorrowRetiresOnOwnerAndSharesNotification) {
  using namespace std::chrono_literals;
  std::promise<std::thread::id> destroyed;
  auto result = destroyed.get_future();
  auto resources_owner = Ros2TransportResources::create("owner_retirement_test", "", 1);
  auto &resources = *resources_owner;
  std::atomic<int> notifications{0};
  resources.set_owner_notification([&] { ++notifications; });
  resources.start();
  std::thread::id owner;
  std::shared_ptr<RetirementProbe> handle;
  resources.invoke([&] {
    owner = std::this_thread::get_id();
    handle = resources.retain_entity(std::make_shared<RetirementProbe>(destroyed));
  });
  auto borrow = handle;
  handle.reset();
  EXPECT_EQ(result.wait_for(0ms), std::future_status::timeout);
  borrow.reset();
  ASSERT_EQ(result.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(result.get(), owner);
  ASSERT_TRUE(resources.notify_owner());
  resources.invoke([] {});
  EXPECT_GE(notifications.load(), 1);
  resources.quiesce();
}

TEST(Ros2TransportOwner, EntityRetirementAfterJoinAndOwnerDestruction) {
  std::promise<std::thread::id> destroyed;
  auto result = destroyed.get_future();
  std::shared_ptr<RetirementProbe> handle;
  {
    auto resources_owner = Ros2TransportResources::create("owner_retirement_join_test", "", 1);
    auto &resources = *resources_owner;
    handle = resources.retain_entity(std::make_shared<RetirementProbe>(destroyed));
    resources.quiesce();
  }
  const auto caller = std::this_thread::get_id();
  handle.reset();
  EXPECT_EQ(result.get(), caller);
}

TEST(Ros2TransportOwner, FailedRetirementNotificationRetainsUntilJoin) {
  auto resources_owner = Ros2TransportResources::create("owner_retirement_trigger_failure", "", 1);
  auto &resources = *resources_owner;
  std::promise<std::thread::id> destroyed;
  auto result = destroyed.get_future();
  auto handle = resources.retain_entity(std::make_shared<RetirementProbe>(destroyed));
  Ros2TransportOwnerTestPeer::fail_trigger(resources);
  handle.reset();
  const auto caller = std::this_thread::get_id();
  resources.quiesce();
  EXPECT_EQ(result.get(), caller);
}

TEST(Ros2TransportOwner, RunningNotifierFailureRetiresAfterJoin) {
  std::promise<std::thread::id> destroyed;
  auto result = destroyed.get_future();
  auto resources_owner = Ros2TransportResources::create("owner_running_retirement_fault", "", 1);
  auto &resources = *resources_owner;
  resources.start();
  auto handle = resources.retain_entity(std::make_shared<RetirementProbe>(destroyed));
  Ros2TransportOwnerTestPeer::fail_trigger(resources);
  handle.reset();
  EXPECT_FALSE(resources.executor_failure_started());
  const auto closer = std::this_thread::get_id();
  resources.quiesce();
  EXPECT_EQ(result.get(), closer);
}

TEST(Ros2TransportOwner, RetirementRacingStartRunsOnOwner) {
  using namespace std::chrono_literals;
  std::promise<std::thread::id> destroyed;
  auto result = destroyed.get_future();
  auto resources_owner = Ros2TransportResources::create("owner_retirement_start_race", "", 1);
  auto &resources = *resources_owner;
  auto handle = resources.retain_entity(std::make_shared<RetirementProbe>(destroyed));
  auto starter = std::async(std::launch::async, [&] { resources.start(); });
  handle.reset();
  starter.get();
  std::thread::id owner;
  resources.invoke([&] { owner = std::this_thread::get_id(); });
  const auto ready = result.wait_for(3s);
  resources.quiesce();
  ASSERT_EQ(ready, std::future_status::ready);
  EXPECT_EQ(result.get(), owner);
}

TEST(Ros2TransportOwner, PendingRetirementDrainsAfterUnstartedJoin) {
  using namespace std::chrono_literals;
  auto resources_owner = Ros2TransportResources::create("owner_retirement_unstarted_test", "", 1);
  auto &resources = *resources_owner;
  std::promise<std::thread::id> destroyed;
  auto result = destroyed.get_future();
  auto handle = resources.retain_entity(std::make_shared<RetirementProbe>(destroyed));
  handle.reset();
  EXPECT_EQ(result.wait_for(0ms), std::future_status::timeout);
  const auto caller = std::this_thread::get_id();
  resources.quiesce();
  EXPECT_EQ(result.get(), caller);
}

TEST(Ros2TransportOwner, RetirementNotificationDoesNotHoldRetirementLock) {
  using namespace std::chrono_literals;
  auto resources_owner = Ros2TransportResources::create("owner_retirement_unlock_test", "", 1);
  auto &resources = *resources_owner;
  std::promise<std::thread::id> first_destroyed, second_destroyed;
  auto first = resources.retain_entity(std::make_shared<RetirementProbe>(first_destroyed));
  auto second = resources.retain_entity(std::make_shared<RetirementProbe>(second_destroyed));
  std::promise<void> two_entered, release;
  auto both = two_entered.get_future();
  auto gate = release.get_future().share();
  std::atomic<int> entered{0};
  Ros2TransportOwnerTestPeer::set_trigger(resources, [&] {
    if (entered.fetch_add(1) + 1 == 2) two_entered.set_value();
    gate.wait();
  });
  std::thread a([value = std::move(first)]() mutable { value.reset(); });
  std::thread b([value = std::move(second)]() mutable { value.reset(); });
  const auto status = both.wait_for(3s);
  release.set_value();
  a.join();
  b.join();
  resources.quiesce();
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_EQ(first_destroyed.get_future().wait_for(0ms), std::future_status::ready);
  EXPECT_EQ(second_destroyed.get_future().wait_for(0ms), std::future_status::ready);
}

TEST(Ros2TransportOwner, NestedRetirementRunsOnOwner) {
  using namespace std::chrono_literals;
  std::promise<std::thread::id> destroyed;
  auto result = destroyed.get_future();
  auto resources_owner = Ros2TransportResources::create("owner_nested_retirement_test", "", 1);
  auto &resources = *resources_owner;
  resources.start();
  std::thread::id owner;
  std::shared_ptr<NestedRetirementProbe> outer;
  resources.invoke([&] {
    owner = std::this_thread::get_id();
    auto inner = resources.retain_entity(std::make_shared<RetirementProbe>(destroyed));
    outer = resources.retain_entity(
        std::make_shared<NestedRetirementProbe>(std::move(inner)));
  });
  outer.reset();
  const auto ready = result.wait_for(3s);
  resources.quiesce();
  ASSERT_EQ(ready, std::future_status::ready);
  EXPECT_EQ(result.get(), owner);
}

TEST(Ros2TransportOwner, NestedRetirementAfterJoinRunsInline) {
  std::promise<std::thread::id> destroyed;
  auto result = destroyed.get_future();
  std::shared_ptr<NestedRetirementProbe> outer;
  {
    auto resources_owner = Ros2TransportResources::create("owner_nested_retirement_join", "", 1);
    auto &resources = *resources_owner;
    auto inner = resources.retain_entity(std::make_shared<RetirementProbe>(destroyed));
    outer = resources.retain_entity(
        std::make_shared<NestedRetirementProbe>(std::move(inner)));
    resources.quiesce();
  }
  const auto caller = std::this_thread::get_id();
  outer.reset();
  EXPECT_EQ(result.get(), caller);
}

// Records whether, while it is being finalized, a callback on another owner
// could release a borrow of the same transport (which takes the retirement
// mutex) within a bound.
struct ReleaseDuringFinalizationProbe {
  ReleaseDuringFinalizationProbe(std::promise<void> &drop, std::future<void> &dropped,
                                 std::atomic<int> &outcome)
      : drop(drop), dropped(dropped), outcome(outcome) {}
  ~ReleaseDuringFinalizationProbe() {
    using namespace std::chrono_literals;
    drop.set_value();
    outcome.store(dropped.wait_for(2s) == std::future_status::ready ? 1 : 2);
  }
  std::promise<void> &drop;
  std::future<void> &dropped;
  std::atomic<int> &outcome;
};

TEST(Ros2TransportOwner, FinalizationDoesNotHoldRetirementLockAgainstCallbacks) {
  using namespace std::chrono_literals;
  auto first_owner = Ros2TransportResources::create("finalizer_lock_first", "", 1);
  auto second_owner = Ros2TransportResources::create("finalizer_lock_second", "", 1);
  second_owner->start();
  std::promise<std::thread::id> borrowed_destroyed;
  auto borrowed_result = borrowed_destroyed.get_future();
  auto borrowed = first_owner->retain_entity(std::make_shared<RetirementProbe>(borrowed_destroyed));
  std::promise<void> drop;
  auto drop_signal = drop.get_future();
  std::promise<void> dropped;
  auto dropped_signal = dropped.get_future();
  std::promise<void> holding;
  auto holding_signal = holding.get_future();
  // A task on the second owner holds the last reference to a borrow of the
  // first transport, and releases it when the finalizer asks.
  std::thread callback_caller([&, held = std::move(borrowed)]() mutable {
    second_owner->invoke([&] {
      holding.set_value();
      if (drop_signal.wait_for(3s) == std::future_status::ready) {
        held.reset();
        dropped.set_value();
      }
    });
  });
  ASSERT_EQ(holding_signal.wait_for(3s), std::future_status::ready);
  std::atomic<int> outcome{0};
  auto finalized = first_owner->retain_entity(
      std::make_shared<ReleaseDuringFinalizationProbe>(drop, dropped_signal, outcome));
  finalized.reset(); // Pending: the first owner never started.
  const auto closer = std::this_thread::get_id();
  first_owner->quiesce();
  callback_caller.join();
  second_owner->quiesce();
  EXPECT_EQ(outcome.load(), 1);
  ASSERT_EQ(borrowed_result.wait_for(0ms), std::future_status::ready);
  EXPECT_EQ(borrowed_result.get(), closer);
}

TEST(Ros2TransportOwner, CloseDrainsRetirementQueuedDuringActiveTask) {
  using namespace std::chrono_literals;
  std::promise<std::thread::id> destroyed;
  auto result = destroyed.get_future();
  std::promise<void> entered;
  auto entered_result = entered.get_future();
  std::promise<void> release;
  auto release_result = release.get_future().share();
  auto resources_owner = Ros2TransportResources::create("owner_close_retirement_test", "", 1);
  auto &resources = *resources_owner;
  resources.start();
  std::thread::id owner;
  std::shared_ptr<RetirementProbe> handle;
  resources.invoke([&] {
    owner = std::this_thread::get_id();
    handle = resources.retain_entity(std::make_shared<RetirementProbe>(destroyed));
  });
  auto active = std::async(std::launch::async, [&] {
    resources.invoke([&] { entered.set_value(); release_result.wait(); });
  });
  const auto started = entered_result.wait_for(3s);
  handle.reset();
  auto closing = std::async(std::launch::async, [&] {
    const auto closer = std::this_thread::get_id();
    resources.quiesce();
    return closer;
  });
  release.set_value();
  active.get();
  const auto closer = closing.get();
  ASSERT_EQ(started, std::future_status::ready);
  ASSERT_EQ(result.wait_for(0ms), std::future_status::ready);
  const auto actual = result.get();
  EXPECT_TRUE(actual == owner || actual == closer);
}

TEST(Ros2TransportOwner, CloseSlotPrecedesOccupiedOrdinarySlot) {
  auto resources_owner = Ros2TransportResources::create("owner_occupied_slots_test", "", 1);
  auto &resources = *resources_owner;
  EXPECT_EQ(Ros2TransportOwnerTestPeer::drive_occupied_slots(resources),
            (std::vector<int>{2, 1}));
}

TEST(Ros2TransportOwner, OwnerNotificationCoalescesBeforeFirstPump) {
  auto resources_owner = Ros2TransportResources::create("owner_notification_test", "", 1);
  auto &resources = *resources_owner;
  int calls = 0;
  resources.set_owner_notification([&] { ++calls; });
  ASSERT_TRUE(resources.notify_owner());
  ASSERT_TRUE(resources.notify_owner());
  for (int step = 0; step < 1000 && calls == 0; ++step)
    Ros2TransportOwnerTestPeer::pump_once(resources);
  EXPECT_EQ(calls, 1);
  for (int step = 0; step < 10; ++step)
    Ros2TransportOwnerTestPeer::pump_once(resources);
  EXPECT_EQ(calls, 1);
}

TEST(Ros2TransportOwner, ThrowingOwnerNotificationClearsDispatchAccounting) {
  using namespace std::chrono_literals;
  std::promise<void> observed;
  auto reported = observed.get_future();
  auto resources_owner = Ros2TransportResources::create("owner_notification_fault_test", "", 1);
  auto &resources = *resources_owner;
  resources.set_owner_notification(
      [] { throw std::runtime_error("notification failed"); });
  resources.set_executor_fault_observer(
      [&](std::string_view) { observed.set_value(); });
  resources.start();
  ASSERT_TRUE(resources.notify_owner());
  ASSERT_EQ(reported.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(resources.executor_fault(),
            std::optional<std::string>("notification failed"));
  EXPECT_TRUE(Ros2TransportOwnerTestPeer::notification_idle(resources));
  resources.quiesce();
}

TEST(Ros2TransportOwner, CreationRefusesWhileFaultObserverIsStillRunning) {
  using namespace std::chrono_literals;
  std::promise<void> observer_entered;
  auto observer_started = observer_entered.get_future();
  std::promise<void> release_observer;
  auto release = release_observer.get_future().share();
  auto resources_owner = Ros2TransportResources::create("owner_creation_fault_test", "", 1);
  auto &resources = *resources_owner;
  resources.set_owner_notification([] { throw std::runtime_error("test fault"); });
  resources.set_executor_fault_observer([&](std::string_view) {
    observer_entered.set_value();
    release.wait();
  });
  resources.start();
  const bool notified = resources.notify_owner();
  const auto observed = observer_started.wait_for(3s);
  std::atomic<bool> created{false};
  auto attempt = std::async(std::launch::async, [&] {
    try {
      resources.invoke_creation([&] { created = true; });
      return std::string("no_error");
    } catch (const core::TypedError &error) {
      return error.code();
    }
  });
  const auto returned = attempt.wait_for(3s);
  std::atomic<bool> ordinary_ran{false};
  auto ordinary = std::async(std::launch::async, [&] {
    resources.invoke([&] { ordinary_ran = true; });
  });
  const auto ordinary_returned = ordinary.wait_for(3s);
  release_observer.set_value();
  const auto code = attempt.get();
  ordinary.get();
  resources.quiesce();
  ASSERT_TRUE(notified);
  ASSERT_EQ(observed, std::future_status::ready);
  ASSERT_EQ(returned, std::future_status::ready);
  EXPECT_EQ(ordinary_returned, std::future_status::ready);
  EXPECT_EQ(code, "ros_transport_executor_failed");
  EXPECT_FALSE(created.load());
  EXPECT_TRUE(ordinary_ran.load());
  try {
    resources.invoke_creation([&] { created = true; });
    ADD_FAILURE() << "faulted owner accepted creation after join";
  } catch (const core::TypedError &error) {
    EXPECT_EQ(error.code(), "ros_transport_executor_failed");
  }
  EXPECT_FALSE(created.load());
}

TEST(Ros2TransportOwner, CreationRunsBeforeStartAndOnRunningOwner) {
  auto resources_owner = Ros2TransportResources::create("owner_creation_affinity_test", "", 1);
  auto &resources = *resources_owner;
  const auto caller = std::this_thread::get_id();
  std::thread::id before_start;
  std::thread::id running;
  resources.invoke_creation([&] { before_start = std::this_thread::get_id(); });
  resources.start();
  resources.invoke_creation([&] { running = std::this_thread::get_id(); });
  resources.quiesce();
  EXPECT_EQ(before_start, caller);
  EXPECT_NE(running, caller);
}

TEST(Ros2TransportOwner, CreationRefusesWhileClosingThenRunsAfterJoin) {
  using namespace std::chrono_literals;
  auto resources_owner = Ros2TransportResources::create("owner_creation_close_test", "", 1);
  auto &resources = *resources_owner;
  resources.start();
  Ros2TransportOwnerTestPeer::hold_foreign_invocation(resources);
  auto closing = std::async(std::launch::async, [&] { resources.quiesce(); });
  const bool exited = Ros2TransportOwnerTestPeer::wait_owner_exit(resources);
  bool created = false;
  std::string code;
  std::string reason;
  if (exited) {
    try {
      resources.invoke_creation([&] { created = true; });
    } catch (const core::TypedError &error) {
      code = error.code();
      const auto found = error.details().find("reason");
      if (found != error.details().end()) reason = found->second;
    }
  }
  Ros2TransportOwnerTestPeer::release_foreign_invocation(resources);
  closing.get();
  ASSERT_TRUE(exited);
  EXPECT_EQ(code, "component_not_ready");
  EXPECT_EQ(reason, "owner_closing");
  EXPECT_FALSE(created);
  const auto caller = std::this_thread::get_id();
  std::thread::id actual;
  resources.invoke_creation([&] { actual = std::this_thread::get_id(); });
  EXPECT_EQ(actual, caller);
}

TEST(Ros2TransportOwner, NestedOwnerCreationRefusesDuringClose) {
  using namespace std::chrono_literals;
  std::promise<void> entered;
  auto entered_result = entered.get_future();
  std::promise<void> proceed;
  auto proceed_result = proceed.get_future().share();
  auto resources_owner = Ros2TransportResources::create("owner_nested_creation_close", "", 1);
  auto &resources = *resources_owner;
  resources.start();
  bool created = false;
  std::string code;
  auto active = std::async(std::launch::async, [&] {
    resources.invoke([&] {
      entered.set_value();
      proceed_result.wait();
      try {
        resources.invoke_creation([&] { created = true; });
      } catch (const core::TypedError &error) {
        code = error.code();
      }
    });
  });
  const auto ready = entered_result.wait_for(3s);
  auto closing = std::async(std::launch::async, [&] { resources.quiesce(); });
  const bool stopping = Ros2TransportOwnerTestPeer::wait_stopping(resources);
  proceed.set_value();
  active.get();
  closing.get();
  ASSERT_EQ(ready, std::future_status::ready);
  ASSERT_TRUE(stopping);
  EXPECT_EQ(code, "component_not_ready");
  EXPECT_FALSE(created);
}

TEST(Ros2TransportOwner, QueuedCreationRefusesBeforeBlockedOwnerExits) {
  using namespace std::chrono_literals;
  std::promise<void> entered;
  auto entered_result = entered.get_future();
  std::promise<void> proceed;
  auto proceed_result = proceed.get_future().share();
  auto resources_owner = Ros2TransportResources::create("owner_queued_creation_close", "", 1);
  auto &resources = *resources_owner;
  resources.start();
  auto active = std::async(std::launch::async, [&] {
    resources.invoke([&] { entered.set_value(); proceed_result.wait(); });
  });
  const auto ready = entered_result.wait_for(3s);
  std::atomic<bool> created{false};
  auto creation = std::async(std::launch::async, [&] {
    try {
      resources.invoke_creation([&] { created = true; });
      return std::string("no_error");
    } catch (const core::TypedError &error) {
      return error.code();
    }
  });
  const bool queued = Ros2TransportOwnerTestPeer::wait_ordinary_slot(resources);
  auto closing = std::async(std::launch::async, [&] { resources.quiesce(); });
  const auto returned = creation.wait_for(3s);
  proceed.set_value();
  const auto code = creation.get();
  active.get();
  closing.get();
  ASSERT_EQ(ready, std::future_status::ready);
  ASSERT_TRUE(queued);
  EXPECT_EQ(returned, std::future_status::ready);
  EXPECT_EQ(code, "component_not_ready");
  EXPECT_FALSE(created.load());
}

TEST(Ros2TransportOwner,
     NotificationBeforeFirstPumpSurvivesEpochEntityCreation) {
  using Action = control_msgs::action::FollowJointTrajectory;
  using State = control_msgs::msg::JointTrajectoryControllerState;
  auto resources_owner = Ros2TransportResources::create("owner_first_pump_test", "", 1);
  auto &resources = *resources_owner;
  // The resource constructor added the node to the executor already, as in
  // epoch construction. Create the epoch entities before the first pump.
  const auto subscription = resources.node()->create_subscription<State>(
      "/owner_first_pump/controller_state", rclcpp::QoS(10),
      [](State::ConstSharedPtr) {});
  const auto action = rclcpp_action::create_client<Action>(
      resources.node(), "/owner_first_pump/follow_joint_trajectory");
  ASSERT_NE(subscription, nullptr);
  ASSERT_NE(action, nullptr);
  int calls = 0;
  Ros2TransportOwnerTestPeer::pump_one_task(resources, calls);
  EXPECT_EQ(calls, 1);
}

TEST(Ros2TransportOwner, NotificationSurvivesRecollectionWithReadyCallback) {
  using Action = control_msgs::action::FollowJointTrajectory;
  using State = control_msgs::msg::JointTrajectoryControllerState;
  using namespace std::chrono_literals;
  auto resources_owner = Ros2TransportResources::create("owner_recollect_pump_test", "", 1);
  auto &resources = *resources_owner;
  auto old_subscription = resources.node()->create_subscription<State>(
      "/owner_recollect/old_state", rclcpp::QoS(10),
      [](State::ConstSharedPtr) {});
  std::atomic<int> calls{0};
  std::atomic<int> callbacks{0};
  resources.set_owner_notification([&] { ++calls; });
  // spin_once discards unconsumed ready work; the owner uses continuous spin.
  resources.start();
  old_subscription.reset();
  const auto subscription = resources.node()->create_subscription<State>(
      "/owner_recollect/new_state", rclcpp::QoS(10),
      [](State::ConstSharedPtr) {});
  const auto action = rclcpp_action::create_client<Action>(
      resources.node(), "/owner_recollect/follow_joint_trajectory");
  const auto timer = resources.node()->create_wall_timer(5ms, [&] { ++callbacks; });
  const bool notified = resources.notify_owner(); // Exactly one trigger.
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while ((calls.load() == 0 || callbacks.load() == 0) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  timer->cancel();
  resources.quiesce();
  EXPECT_NE(subscription, nullptr);
  EXPECT_NE(action, nullptr);
  EXPECT_TRUE(notified);
  EXPECT_EQ(calls.load(), 1);
  EXPECT_GE(callbacks.load(), 1);
}

TEST(Ros2TransportOwner, ForeignWorkAndReentryReturnActualResult) {
  auto resources_owner = Ros2TransportResources::create("owner_handoff_test", "", 1);
  auto &resources = *resources_owner;
  resources.invoke([&] {
    EXPECT_FALSE(resources.on_owner_thread());
    resources.invoke([&] { EXPECT_FALSE(resources.on_owner_thread()); });
  });
  resources.start();
  const auto caller = std::this_thread::get_id();
  std::thread::id owner;
  int result = 0;
  resources.invoke([&] {
    EXPECT_TRUE(resources.on_owner_thread());
    owner = std::this_thread::get_id();
    resources.invoke([&] {
      EXPECT_TRUE(resources.on_owner_thread());
      EXPECT_EQ(std::this_thread::get_id(), owner);
      result = 42;
    });
  });
  EXPECT_NE(owner, caller);
  EXPECT_EQ(result, 42);
  resources.invoke([&] { result = 7; },
                   Ros2TransportResources::DispatchOrder::kClose);
  resources.invoke([&] { EXPECT_EQ(result, 7); });
  EXPECT_THROW(resources.invoke([] { throw std::runtime_error("work failed"); }),
               std::runtime_error);
  resources.quiesce();
  resources.invoke([&] {
    EXPECT_FALSE(resources.on_owner_thread());
    result = 99;
  });
  EXPECT_EQ(result, 99);
}

TEST(Ros2TransportOwner, SlowOwnerWorkLatchesOneDiagnosticAndCompletes) {
  using namespace std::chrono_literals;
  auto resources_owner = Ros2TransportResources::create("owner_stall_bound_test", "", 1);
  auto &resources = *resources_owner;
  Ros2TransportOwnerTestPeer::set_wait_bound(resources, 50ms);
  resources.start();
  int calls = 0;
  resources.invoke([&] { ++calls; });
  EXPECT_FALSE(resources.owner_stall());

  testing::internal::CaptureStderr();
  const auto started = std::chrono::steady_clock::now();
  resources.invoke([&] {
    std::this_thread::sleep_for(200ms);
    ++calls;
  });
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const auto first = resources.owner_stall();
  resources.invoke([&] {
    std::this_thread::sleep_for(200ms);
    ++calls;
  });
  const auto second = resources.owner_stall();
  resources.invoke([&] { ++calls; });
  const auto diagnostic = testing::internal::GetCapturedStderr();

  EXPECT_GE(elapsed, 200ms);
  EXPECT_EQ(calls, 4);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
  EXPECT_EQ(first->owner, "/owner_stall_bound_test");
  EXPECT_NE(first->task.find("SlowOwnerWorkLatchesOneDiagnosticAndCompletes"),
            std::string::npos);
  const std::string marker = "manyforge_fault code=component_owner_stalled";
  const auto occurrence = diagnostic.find(marker);
  ASSERT_NE(occurrence, std::string::npos);
  EXPECT_EQ(diagnostic.find(marker, occurrence + marker.size()), std::string::npos);
  EXPECT_NE(diagnostic.find("owner=/owner_stall_bound_test"), std::string::npos);
  EXPECT_NE(diagnostic.find("bound_ms=50"), std::string::npos);
  resources.quiesce();
}

TEST(Ros2TransportOwner, PostOwnerExitFallbackPropagatesWorkException) {
  auto resources_owner = Ros2TransportResources::create("owner_exited_exception_test", "", 1);
  auto &resources = *resources_owner;
  resources.quiesce();
  int calls = 0;
  EXPECT_THROW(resources.invoke([&] {
                 ++calls;
                 throw std::runtime_error("fallback work failed");
               }),
               std::runtime_error);
  EXPECT_EQ(calls, 1);
}

TEST(Ros2TransportOwner, ThrowingCallbackReportsFaultAndOwnerExits) {
  using namespace std::chrono_literals;
  std::promise<void> observed;
  auto reported = observed.get_future();
  std::atomic<int> notifications{0};
  std::atomic<bool> armed{false};
  auto resources_owner = Ros2TransportResources::create("owner_callback_fault_test", "", 1);
  auto &resources = *resources_owner;
  resources.set_executor_fault_observer([&](std::string_view) {
    if (++notifications == 1)
      observed.set_value();
  });
  const auto timer = resources.node()->create_wall_timer(1ms, [&] {
    if (armed.load())
      throw std::runtime_error("owner callback failure");
  });
  resources.start();
  armed.store(true);
  ASSERT_EQ(reported.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(resources.executor_fault(),
            std::optional<std::string>("owner callback failure"));
  const auto caller = std::this_thread::get_id();
  resources.invoke([&] {
    EXPECT_FALSE(resources.on_owner_thread());
    EXPECT_EQ(std::this_thread::get_id(), caller);
  });
  EXPECT_EQ(notifications.load(), 1);
  resources.quiesce();
}

TEST(Ros2TransportOwner, TriggerFailureReportsFaultThenRunsInlineOnce) {
  using namespace std::chrono_literals;
  std::promise<void> observed;
  auto reported = observed.get_future();
  std::atomic<int> notifications{0};
  auto resources_owner = Ros2TransportResources::create("owner_notifier_fault_test", "", 1);
  auto &resources = *resources_owner;
  Ros2TransportOwnerTestPeer::fail_trigger(resources);
  resources.set_executor_fault_observer([&](std::string_view) {
    if (++notifications == 1)
      observed.set_value();
  });
  resources.start();
  const auto caller = std::this_thread::get_id();
  int calls = 0;
  resources.invoke([&] {
    EXPECT_FALSE(resources.on_owner_thread());
    EXPECT_EQ(std::this_thread::get_id(), caller);
    ++calls;
  });
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(reported.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(resources.executor_fault(),
            std::optional<std::string>("test notifier failure"));
  resources.quiesce();
  EXPECT_EQ(notifications.load(), 1);
}

// Use the executor's normal notification exception path to fault an owner.
void FaultOwner(Ros2TransportResources &resources) {
  using namespace std::chrono_literals;
  auto observed = std::make_shared<std::promise<void>>();
  auto reported = observed->get_future();
  resources.set_owner_notification([] { throw std::runtime_error("rebuild test fault"); });
  resources.set_executor_fault_observer([observed](std::string_view) { observed->set_value(); });
  resources.start();
  if (!resources.notify_owner() || reported.wait_for(3s) != std::future_status::ready)
    throw std::runtime_error("failed to inject executor fault");
}

TEST(Ros2TransportOwner, RebuildRequiresFaultAndPreservesHealthyOwner) {
  auto healthy_owner = Ros2TransportResources::create("rebuild_healthy", "", 1);
  auto &healthy = *healthy_owner;
  int calls = 0;
  EXPECT_THROW(Ros2TransportResources::rebuild_faulted(
                   healthy, std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {},
                   [&] { ++calls; return Ros2TransportResources::create(
                                      "unused_replacement", "", 1); },
                   [](Ros2TransportResources &) {}),
               std::logic_error);
  EXPECT_EQ(calls, 0);
  healthy.start();
  healthy.quiesce();
}

TEST(Ros2TransportOwner, RebuildCanReleaseAlreadyJoinedOwnerAfterDeadline) {
  auto owner = Ros2TransportResources::create("rebuild_expired_owner", "", 1);
  FaultOwner(*owner);
  owner->quiesce();
  bool released = false;
  auto replacement = Ros2TransportResources::rebuild_faulted(
      *owner, std::chrono::steady_clock::now() - std::chrono::milliseconds(1),
      [&] { released = true; },
      [] { return Ros2TransportResources::create("replacement_after_join", "", 1); },
      [](Ros2TransportResources &replacement) { replacement.start(); });
  EXPECT_TRUE(released);
  EXPECT_NE(replacement, nullptr);
}

TEST(Ros2TransportOwner, RebuildRetainsOldEntitiesUntilConcurrentQuiesceFinishes) {
  using namespace std::chrono_literals;
  auto owner = Ros2TransportResources::create("rebuild_concurrent_owner", "", 1);
  std::promise<void> observer_entered;
  auto entered = observer_entered.get_future();
  std::promise<void> release_observer;
  auto release = release_observer.get_future().share();
  owner->set_owner_notification([] { throw std::runtime_error("rebuild fault"); });
  owner->set_executor_fault_observer([&](std::string_view) {
    observer_entered.set_value();
    release.wait();
  });
  owner->start();
  const bool notified = owner->notify_owner();
  const bool faulted = entered.wait_for(2s) == std::future_status::ready;
  auto winner = std::async(std::launch::async, [&] { owner->quiesce(); });
  const auto until = std::chrono::steady_clock::now() + 2s;
  while (!Ros2TransportOwnerTestPeer::quiesce_started(*owner) &&
         std::chrono::steady_clock::now() < until) std::this_thread::yield();
  bool released = false;
  bool made = false;
  std::string code;
  try {
    (void)Ros2TransportResources::rebuild_faulted(
        *owner, std::chrono::steady_clock::now() + 20ms,
        [&] { released = true; },
        [&] { made = true; return Ros2TransportResources::create("unused_rebuild", "", 1); },
        [](Ros2TransportResources&) {});
  } catch (const core::TypedError& error) { code = error.code(); }
    catch (const std::exception& error) { code = error.what(); }
  const bool retained = owner->cached_context_valid() && !owner->joined();
  release_observer.set_value();
  winner.get();
  const bool joined_by_winner = owner->joined();
  if (!joined_by_winner)
    (void)owner->quiesce_until(std::chrono::steady_clock::now() + 2s);
  EXPECT_TRUE(notified);
  EXPECT_TRUE(faulted);
  EXPECT_EQ(code, "ros_transport_quiesce_timeout");
  EXPECT_FALSE(released);
  EXPECT_FALSE(made);
  EXPECT_TRUE(retained);
  EXPECT_TRUE(joined_by_winner);
}

TEST(Ros2TransportOwner, RebuildKeepsOldBorrowUntilLastRelease) {
  std::promise<std::thread::id> destroyed;
  auto destruction = destroyed.get_future();
  auto old = Ros2TransportResources::create("rebuild_old", "", 1);
  auto borrow = old->retain_entity(std::make_shared<RetirementProbe>(destroyed));
  FaultOwner(*old);
  int prepared = 0;
  auto replacement = Ros2TransportResources::rebuild_faulted(
      *old, std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {},
      [] { return Ros2TransportResources::create("rebuild_new", "", 1); },
      [&](Ros2TransportResources &owner) { ++prepared; owner.start(); });
  EXPECT_EQ(prepared, 1);
  EXPECT_TRUE(replacement->started());
  EXPECT_FALSE(replacement->executor_fault());
  old.reset();
  EXPECT_EQ(destruction.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
  const auto release_thread = std::this_thread::get_id();
  borrow.reset();
  ASSERT_EQ(destruction.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_EQ(destruction.get(), release_thread);
  replacement->quiesce();
}

TEST(Ros2TransportOwner, RebuildReleasesOldEntitiesAfterJoinBeforeFactory) {
  std::promise<std::thread::id> destroyed;
  auto destruction = destroyed.get_future();
  auto old_owner = Ros2TransportResources::create("rebuild_release_order", "", 1);
  auto &old = *old_owner;
  auto entity = old.retain_entity(std::make_shared<RetirementProbe>(destroyed));
  FaultOwner(old);
  std::vector<int> order;
  auto replacement = Ros2TransportResources::rebuild_faulted(
      old, std::chrono::steady_clock::now() + std::chrono::seconds(2),
      [&] { order.push_back(1); entity.reset(); },
      [&] {
        order.push_back(2);
        EXPECT_EQ(destruction.wait_for(std::chrono::milliseconds(0)),
                  std::future_status::ready);
        return Ros2TransportResources::create("rebuild_order_new", "", 1);
      },
      [&](Ros2TransportResources &owner) { order.push_back(3); owner.start(); });
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(destruction.get(), std::this_thread::get_id());
}

TEST(Ros2TransportOwner, RebuildFailureRetainsFaultAndCleansCandidate) {
  auto old_owner = Ros2TransportResources::create("rebuild_failed_old", "", 1);
  auto &old = *old_owner;
  FaultOwner(old);
  int factories = 0;
  EXPECT_THROW(Ros2TransportResources::rebuild_faulted(
                   old, std::chrono::steady_clock::now() + std::chrono::seconds(2), [] { throw std::runtime_error("release failed"); },
                   [&] { ++factories; return Ros2TransportResources::create(
                                      "unused_release_failure", "", 1); },
                   [](Ros2TransportResources &) {}),
               std::runtime_error);
  EXPECT_EQ(factories, 0);
  EXPECT_THROW(Ros2TransportResources::rebuild_faulted(
                   old, std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {},
                   []() -> std::shared_ptr<Ros2TransportResources> {
                     throw std::runtime_error("factory failed");
                   },
                   [](Ros2TransportResources &) {}),
               std::runtime_error);
  std::promise<std::thread::id> destroyed;
  auto destruction = destroyed.get_future();
  EXPECT_THROW(Ros2TransportResources::rebuild_faulted(
                   old, std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {},
                   [] { return Ros2TransportResources::create(
                                      "rebuild_partial", "", 1); },
                   [&](Ros2TransportResources &candidate) {
                     auto entity = candidate.retain_entity(std::make_shared<RetirementProbe>(destroyed));
                     throw std::runtime_error("prepare failed");
                   }),
               std::runtime_error);
  EXPECT_EQ(old.executor_fault(), std::optional<std::string>("rebuild test fault"));
  EXPECT_EQ(destruction.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_THROW(Ros2TransportResources::rebuild_faulted(
                   old, std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {},
                   [] { return Ros2TransportResources::create(
                                      "rebuild_faulted_candidate", "", 1); },
                   [](Ros2TransportResources &candidate) { FaultOwner(candidate); }),
               std::runtime_error);
  EXPECT_EQ(old.executor_fault(), std::optional<std::string>("rebuild test fault"));
}

TEST(Ros2TransportOwner, RebuildRejectsUnstartedOrClosedReplacement) {
  auto old_owner = Ros2TransportResources::create("rebuild_bad_candidate_old", "", 1);
  auto &old = *old_owner;
  FaultOwner(old);
  auto make = [] {
    return Ros2TransportResources::create("rebuild_bad_candidate", "", 1);
  };
  const auto check = [&](auto prepare) {
    try {
      (void)Ros2TransportResources::rebuild_faulted(old, std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {}, make, prepare);
      FAIL() << "unavailable candidate accepted";
    } catch (const core::TypedError &error) {
      EXPECT_EQ(error.code(), "component_not_ready");
    }
  };
  check([](Ros2TransportResources &) {});
  check([](Ros2TransportResources &candidate) { candidate.start(); candidate.quiesce(); });
  EXPECT_EQ(old.executor_fault(), std::optional<std::string>("rebuild test fault"));
}

TEST(Ros2TransportOwner, RebuildFromFaultObserverRefusesSelfJoin) {
  using namespace std::chrono_literals;
  auto old_owner = Ros2TransportResources::create("rebuild_observer", "", 1);
  auto &old = *old_owner;
  std::promise<bool> rejected;
  auto outcome = rejected.get_future();
  old.set_owner_notification([] { throw std::runtime_error("observer fault"); });
  old.set_executor_fault_observer([&](std::string_view) {
    try {
      (void)Ros2TransportResources::rebuild_faulted(
          old, std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {}, [] { return Ros2TransportResources::create("unused", "", 1); },
          [](Ros2TransportResources &) {});
      rejected.set_value(false);
    } catch (const core::TypedError &error) { rejected.set_value(error.code() == "component_not_ready"); }
  });
  old.start();
  ASSERT_TRUE(old.notify_owner());
  ASSERT_EQ(outcome.wait_for(3s), std::future_status::ready);
  EXPECT_TRUE(outcome.get());
  old.quiesce();
}

TEST(Ros2TransportOwner, ExistingAuthoritySerializesExactOwnerRecovery) {
  auto slot = Ros2TransportResources::create("rebuild_slot_old", "", 1);
  FaultOwner(*slot);
  auto expected = slot;
  std::mutex authority;
  int factories = 0;
  const auto recover = [&] {
    // This mutex models an existing authority's serialization. Real callers
    // must not hold locks needed by old callbacks while joining.
    std::lock_guard lock(authority);
    if (slot != expected) return false;
    auto candidate = Ros2TransportResources::rebuild_faulted(
        *expected, std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {},
        [&] { ++factories; return Ros2TransportResources::create(
                                    "rebuild_slot_new", "", 1); },
        [](Ros2TransportResources &owner) { owner.start(); });
    slot = std::move(candidate);
    return true;
  };
  auto first = std::async(std::launch::async, recover);
  auto second = std::async(std::launch::async, recover);
  EXPECT_NE(first.get(), second.get());
  EXPECT_EQ(factories, 1);
  slot->quiesce();
}

// ---------------------------------------------------------------------------
// OD-66: time-critical executor thread.

using TimeCriticalMessage = control_msgs::msg::JointTrajectoryControllerState;

std::shared_ptr<rclcpp::Node> TimeCriticalPeerNode(Ros2TransportResources &resources,
                                                   const char *name) {
  rclcpp::NodeOptions options;
  options.context(resources.node()->get_node_base_interface()->get_context());
  return std::make_shared<rclcpp::Node>(name, options);
}

// Publishes until `received` holds (discovery may drop the first messages).
bool PublishUntil(const rclcpp::Publisher<TimeCriticalMessage>::SharedPtr &publisher,
                  const std::function<bool()> &received) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!received() && std::chrono::steady_clock::now() < deadline) {
    publisher->publish(TimeCriticalMessage{});
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return received();
}

// Releases a blocked callback or task on every exit path, before the owner's
// destructor quiesces (declare it after the owner).
struct ReleaseOnExit final {
  std::atomic<bool> &released;
  ~ReleaseOnExit() { released.store(true); }
};

void WaitReleased(const std::atomic<bool> &released) {
  while (!released.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

TEST(Ros2TransportOwner, TimeCriticalSubscriptionRunsWhileOwnerTimerBlocks) {
  using namespace std::chrono_literals;
  // Callback state outlives the owner; the release guard runs before its quiesce.
  std::atomic<bool> released{false};
  std::atomic<bool> armed{false};
  std::atomic<bool> blocking{false};
  std::atomic<int> received{0};
  std::atomic<bool> on_time_critical{false};
  std::atomic<bool> on_owner{true};
  auto resources_owner = Ros2TransportResources::create("tc_timer_blocked_test", "", 1);
  auto &resources = *resources_owner;
  ReleaseOnExit release{released};
  // An ordinary callback on the owner thread, blocked on a latch.
  const auto timer = resources.node()->create_wall_timer(1ms, [&] {
    if (!armed.load() || blocking.exchange(true)) return;
    WaitReleased(released);
  });
  rclcpp::SubscriptionOptions options;
  options.callback_group = resources.create_time_critical_callback_group();
  ASSERT_TRUE(options.callback_group);
  auto subscription = resources.node()->create_subscription<TimeCriticalMessage>(
      "/manyforge_tc_timer_blocked", rclcpp::QoS(10),
      [&](const TimeCriticalMessage &) {
        on_time_critical.store(resources.on_time_critical_thread());
        on_owner.store(resources.on_owner_thread());
        ++received;
      }, options);
  resources.start();
  armed.store(true);
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!blocking.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  ASSERT_TRUE(blocking.load());
  auto peer = TimeCriticalPeerNode(resources, "tc_timer_blocked_publisher");
  auto publisher = peer->create_publisher<TimeCriticalMessage>("/manyforge_tc_timer_blocked",
                                                               rclcpp::QoS(10));
  EXPECT_TRUE(PublishUntil(publisher, [&] { return received.load() > 0; }));
  EXPECT_FALSE(released.load());  // Delivered while the owner thread was blocked.
  EXPECT_TRUE(on_time_critical.load());
  EXPECT_FALSE(on_owner.load());
  released.store(true);
  resources.quiesce();
  subscription.reset();
}

TEST(Ros2TransportOwner, TimeCriticalSubscriptionRunsWhileDispatchedTaskBlocks) {
  using namespace std::chrono_literals;
  std::atomic<bool> released{false};
  std::atomic<int> received{0};
  std::promise<void> entered;
  auto in_task = entered.get_future();
  auto resources_owner = Ros2TransportResources::create("tc_task_blocked_test", "", 1);
  auto &resources = *resources_owner;
  std::future<void> task;
  ReleaseOnExit release{released};  // Before the task's blocking destructor.
  rclcpp::SubscriptionOptions options;
  options.callback_group = resources.create_time_critical_callback_group();
  auto subscription = resources.node()->create_subscription<TimeCriticalMessage>(
      "/manyforge_tc_task_blocked", rclcpp::QoS(10),
      [&](const TimeCriticalMessage &) { ++received; }, options);
  resources.start();
  // Shorter than the stall bound: a dispatched owner task blocked on a latch.
  task = std::async(std::launch::async, [&] {
    resources.invoke([&] {
      entered.set_value();
      WaitReleased(released);
    });
  });
  ASSERT_EQ(in_task.wait_for(3s), std::future_status::ready);
  auto peer = TimeCriticalPeerNode(resources, "tc_task_blocked_publisher");
  auto publisher = peer->create_publisher<TimeCriticalMessage>("/manyforge_tc_task_blocked",
                                                               rclcpp::QoS(10));
  EXPECT_TRUE(PublishUntil(publisher, [&] { return received.load() > 0; }));
  EXPECT_FALSE(released.load());
  released.store(true);
  task.get();
  resources.quiesce();
  subscription.reset();
}

TEST(Ros2TransportOwner, CreationRunsOnOwnerThreadNotTimeCriticalThread) {
  using namespace std::chrono_literals;
  std::promise<std::thread::id> delivered;
  auto delivery = delivered.get_future();
  std::atomic<bool> first{true};
  auto resources_owner = Ros2TransportResources::create("tc_creation_thread_test", "", 1);
  auto &resources = *resources_owner;
  resources.enable_time_critical_callbacks();
  resources.enable_time_critical_callbacks();  // Idempotent before start.
  resources.start();
  std::thread::id owner_thread;
  resources.invoke([&] { owner_thread = std::this_thread::get_id(); });
  std::thread::id creation_thread;
  bool creation_on_time_critical = true;
  rclcpp::Subscription<TimeCriticalMessage>::SharedPtr subscription;
  // A group added to the running time-critical executor serves its entity.
  resources.invoke_creation([&] {
    creation_thread = std::this_thread::get_id();
    creation_on_time_critical = resources.on_time_critical_thread();
    rclcpp::SubscriptionOptions options;
    options.callback_group = resources.create_time_critical_callback_group();
    ASSERT_TRUE(options.callback_group);
    subscription = resources.retain_entity(
        resources.node()->create_subscription<TimeCriticalMessage>(
            "/manyforge_tc_creation_thread", rclcpp::QoS(10),
            [&](const TimeCriticalMessage &) {
              if (first.exchange(false)) delivered.set_value(std::this_thread::get_id());
            }, options),
        options.callback_group);
  });
  EXPECT_EQ(creation_thread, owner_thread);
  EXPECT_FALSE(creation_on_time_critical);
  auto peer = TimeCriticalPeerNode(resources, "tc_creation_thread_publisher");
  auto publisher = peer->create_publisher<TimeCriticalMessage>("/manyforge_tc_creation_thread",
                                                               rclcpp::QoS(10));
  ASSERT_TRUE(PublishUntil(publisher, [&] {
    return delivery.wait_for(0s) == std::future_status::ready;
  }));
  const auto callback_thread = delivery.get();
  EXPECT_NE(callback_thread, owner_thread);
  EXPECT_EQ(callback_thread, Ros2TransportOwnerTestPeer::time_critical_thread_id(resources));
  subscription.reset();
  resources.quiesce();
}

TEST(Ros2TransportOwner, OwnerWithoutTimeCriticalThreadUsesDefaultGroupAfterStart) {
  auto resources_owner = Ros2TransportResources::create("tc_absent_test", "", 1);
  auto &resources = *resources_owner;
  resources.start();
  EXPECT_FALSE(resources.create_time_critical_callback_group());
  EXPECT_THROW(resources.enable_time_critical_callbacks(), std::logic_error);
  resources.quiesce();
}

TEST(Ros2TransportOwner, QuiesceJoinsBothExecutorThreads) {
  using namespace std::chrono_literals;
  std::atomic<bool> released{false};
  std::atomic<bool> armed{false};
  std::atomic<bool> blocking{false};
  auto resources_owner = Ros2TransportResources::create("tc_quiesce_join_test", "", 1);
  auto &resources = *resources_owner;
  ReleaseOnExit release{released};
  // A time-critical callback that is still running keeps quiesce unresolved.
  // A subscription triggers it: the time-critical executor serves the
  // entities the product puts there (action clients, subscriptions).
  rclcpp::SubscriptionOptions options;
  options.callback_group = resources.create_time_critical_callback_group();
  ASSERT_TRUE(options.callback_group);
  auto subscription = resources.node()->create_subscription<TimeCriticalMessage>(
      "/manyforge_tc_quiesce_join", rclcpp::QoS(10), [&](const TimeCriticalMessage &) {
        if (!armed.load() || blocking.exchange(true)) return;
        WaitReleased(released);
      }, options);
  resources.start();
  armed.store(true);
  auto peer = TimeCriticalPeerNode(resources, "tc_quiesce_join_publisher");
  auto publisher = peer->create_publisher<TimeCriticalMessage>("/manyforge_tc_quiesce_join",
                                                               rclcpp::QoS(10));
  ASSERT_TRUE(PublishUntil(publisher, [&] { return blocking.load(); }));
  EXPECT_FALSE(resources.quiesce_until(std::chrono::steady_clock::now() + 50ms));
  EXPECT_FALSE(resources.joined());
  EXPECT_TRUE(Ros2TransportOwnerTestPeer::owner_joined(resources));
  EXPECT_FALSE(Ros2TransportOwnerTestPeer::time_critical_joined(resources));
  released.store(true);
  resources.quiesce();
  EXPECT_TRUE(resources.joined());
  EXPECT_TRUE(Ros2TransportOwnerTestPeer::owner_joined(resources));
  EXPECT_TRUE(Ros2TransportOwnerTestPeer::time_critical_joined(resources));
}

TEST(Ros2TransportOwner, QuiesceJoinsTimeCriticalThreadWithoutCallbacks) {
  auto resources_owner = Ros2TransportResources::create("tc_idle_join_test", "", 1);
  auto &resources = *resources_owner;
  resources.enable_time_critical_callbacks();
  resources.start();
  resources.quiesce();
  EXPECT_TRUE(resources.joined());
  EXPECT_TRUE(Ros2TransportOwnerTestPeer::time_critical_joined(resources));
}

struct TimeCriticalBundle {
  rclcpp::Subscription<TimeCriticalMessage>::SharedPtr subscription;
  std::unique_ptr<RetirementProbe> probe;
};

TEST(Ros2TransportOwner, TimeCriticalLastReleaseFinalizesOnOwnerThread) {
  using namespace std::chrono_literals;
  std::promise<std::thread::id> destroyed;
  auto finalized = destroyed.get_future();
  std::mutex holder_mutex;
  std::shared_ptr<TimeCriticalBundle> holder;
  std::atomic<bool> dropped_on_time_critical{false};
  auto resources_owner = Ros2TransportResources::create("tc_retirement_test", "", 1);
  auto &resources = *resources_owner;
  resources.enable_time_critical_callbacks();
  resources.start();
  std::thread::id owner_thread;
  resources.invoke([&] { owner_thread = std::this_thread::get_id(); });
  resources.invoke_creation([&] {
    auto group = resources.create_time_critical_callback_group();
    auto bundle = std::make_shared<TimeCriticalBundle>();
    rclcpp::SubscriptionOptions options;
    options.callback_group = group;
    bundle->subscription = resources.node()->create_subscription<TimeCriticalMessage>(
        "/manyforge_tc_retirement", rclcpp::QoS(10),
        [&](const TimeCriticalMessage &) {
          // The last borrow is dropped by a time-critical callback.
          std::shared_ptr<TimeCriticalBundle> drop;
          {
            std::lock_guard lock(holder_mutex);
            drop = std::move(holder);
          }
          if (drop) dropped_on_time_critical.store(resources.on_time_critical_thread());
        }, options);
    bundle->probe = std::make_unique<RetirementProbe>(destroyed);
    std::lock_guard lock(holder_mutex);
    holder = resources.retain_entity(std::move(bundle), std::move(group));
  });
  auto peer = TimeCriticalPeerNode(resources, "tc_retirement_publisher");
  auto publisher = peer->create_publisher<TimeCriticalMessage>("/manyforge_tc_retirement",
                                                               rclcpp::QoS(10));
  ASSERT_TRUE(PublishUntil(publisher, [&] {
    return finalized.wait_for(0s) == std::future_status::ready;
  }));
  EXPECT_TRUE(dropped_on_time_critical.load());
  // Routed to the owner thread after the removal fence, never finalized on
  // the time-critical thread.
  EXPECT_EQ(finalized.get(), owner_thread);
  resources.quiesce();
}

TEST(Ros2TransportOwner, ForeignReleaseOfTimeCriticalRecordFinalizesOnOwnerThread) {
  using namespace std::chrono_literals;
  std::promise<std::thread::id> destroyed;
  auto finalized = destroyed.get_future();
  auto resources_owner = Ros2TransportResources::create("tc_foreign_release_test", "", 1);
  auto &resources = *resources_owner;
  auto group = resources.create_time_critical_callback_group();
  ASSERT_TRUE(group);
  auto borrow = resources.retain_entity(std::make_shared<RetirementProbe>(destroyed),
                                        std::move(group));
  resources.start();
  std::thread::id owner_thread;
  resources.invoke([&] { owner_thread = std::this_thread::get_id(); });
  borrow.reset();
  // The owner removes the group, the time-critical thread acknowledges a
  // rebuild one wait later, then the owner finalizes.
  ASSERT_EQ(finalized.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(finalized.get(), owner_thread);
  resources.quiesce();
}

TEST(Ros2TransportOwner, TimeCriticalCallbackFaultStopsWholeOwner) {
  using namespace std::chrono_literals;
  std::promise<void> observed;
  auto reported = observed.get_future();
  std::atomic<int> notifications{0};
  std::atomic<bool> armed{false};
  auto resources_owner = Ros2TransportResources::create("tc_fault_test", "", 1);
  auto &resources = *resources_owner;
  resources.set_executor_fault_observer([&](std::string_view) {
    if (++notifications == 1) observed.set_value();
  });
  rclcpp::SubscriptionOptions options;
  options.callback_group = resources.create_time_critical_callback_group();
  ASSERT_TRUE(options.callback_group);
  auto subscription = resources.node()->create_subscription<TimeCriticalMessage>(
      "/manyforge_tc_fault", rclcpp::QoS(10), [&](const TimeCriticalMessage &) {
        if (armed.load()) throw std::runtime_error("time-critical callback failure");
      }, options);
  resources.start();
  armed.store(true);
  auto peer = TimeCriticalPeerNode(resources, "tc_fault_publisher");
  auto publisher = peer->create_publisher<TimeCriticalMessage>("/manyforge_tc_fault",
                                                               rclcpp::QoS(10));
  ASSERT_TRUE(PublishUntil(publisher, [&] {
    return reported.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  }));
  EXPECT_TRUE(resources.executor_failure_started());
  EXPECT_EQ(resources.executor_fault(),
            std::optional<std::string>("time-critical callback failure"));
  EXPECT_TRUE(Ros2TransportOwnerTestPeer::wait_owner_exit(resources));
  resources.quiesce();
  EXPECT_EQ(notifications.load(), 1);
  EXPECT_TRUE(Ros2TransportOwnerTestPeer::time_critical_joined(resources));
}

} // namespace
} // namespace manyforge::ros2_control_execution
