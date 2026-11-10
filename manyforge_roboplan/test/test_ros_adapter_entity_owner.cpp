#include <gtest/gtest.h>

#include "ros_assistant_host_link.hpp"
#include "ros_pointcloud_bridge_link.hpp"
#include "ros_simulation_control.hpp"

#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace manyforge::roboplan_backend {
namespace {
using namespace std::chrono_literals;

struct ClockEvents {
  std::mutex mutex;
  std::condition_variable changed;
  std::thread::id called;
  std::thread::id destroyed;
};

class TracedClock final : public core::IClock {
public:
  explicit TracedClock(std::shared_ptr<ClockEvents> events)
      : events_(std::move(events)) {}
  ~TracedClock() override {
    std::lock_guard lock(events_->mutex);
    events_->destroyed = std::this_thread::get_id();
    events_->changed.notify_all();
  }
  core::TimePoint now() const override {
    std::lock_guard lock(events_->mutex);
    events_->called = std::this_thread::get_id();
    events_->changed.notify_all();
    return 42;
  }

private:
  std::shared_ptr<ClockEvents> events_;
};

TEST(RosAdapterEntityOwner,
     CallbacksAndForeignRetirementUseOwnerAndAllowReuse) {
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  auto owner = ros2_control_execution::Ros2TransportResources::create(
      "adapter_owner_test", "", 1, [context] { return context; });
  rclcpp::NodeOptions options;
  options.context(context).use_global_arguments(false);
  auto peer = std::make_shared<rclcpp::Node>("adapter_peer_test", options);
  auto publisher = peer->create_publisher<std_msgs::msg::Header>(
      "/owner_test/received", rclcpp::QoS(1).reliable());
  owner->start();
  std::thread::id owner_thread;
  owner->invoke([&] { owner_thread = std::this_thread::get_id(); });

  auto events = std::make_shared<ClockEvents>();
  auto link = std::make_shared<RosPointCloudBridgeLink>(
      owner->node(), "/owner_test", std::make_shared<TracedClock>(events),
      owner);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (publisher->get_subscription_count() == 0 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_GT(publisher->get_subscription_count(), 0u);
  publisher->publish(std_msgs::msg::Header{});
  {
    std::unique_lock lock(events->mutex);
    ASSERT_TRUE(events->changed.wait_for(
        lock, 5s, [&] { return events->called != std::thread::id{}; }));
    EXPECT_EQ(events->called, owner_thread);
  }
  owner->invoke([] {});
  EXPECT_EQ(link->Observed().last_received_ns, 42);
  std::thread drop([link = std::move(link)]() mutable { link.reset(); });
  drop.join();
  {
    std::unique_lock lock(events->mutex);
    ASSERT_TRUE(events->changed.wait_for(
        lock, 5s, [&] { return events->destroyed != std::thread::id{}; }));
    EXPECT_EQ(events->destroyed, owner_thread);
  }
  auto reused = std::make_shared<RosPointCloudBridgeLink>(
      owner->node(), "/owner_test", std::make_shared<TracedClock>(events),
      owner);
  reused.reset();
  auto simulation = std::make_unique<RosSimulationControl>(
      owner->node(),
      SimulationServices{"/owner_test/get", "/owner_test/set",
                         "/owner_test/reset", 1s, 1s},
      owner);
  std::thread drop_simulation(
      [simulation = std::move(simulation)]() mutable { simulation.reset(); });
  drop_simulation.join();
  owner->quiesce();
  owner.reset();
  publisher.reset();
  peer.reset();
  context->shutdown("adapter owner test complete");
}

TEST(RosAdapterEntityOwner, SimulatorAndPointCloudStageAgainstExactOwner) {
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  auto old_owner = ros2_control_execution::Ros2TransportResources::create(
      "adapter_rebind_old", "", 1, [context] { return context; });
  auto new_owner = ros2_control_execution::Ros2TransportResources::create(
      "adapter_rebind_new", "", 1, [context] { return context; });
  old_owner->start();
  new_owner->start();
  auto events = std::make_shared<ClockEvents>();
  RosPointCloudBridgeLink link(old_owner->node(), "/rebind_cloud",
                              std::make_shared<TracedClock>(events), old_owner);
  RosSimulationControl simulation(
      old_owner->node(),
      SimulationServices{"/rebind/get", "/rebind/set", "/rebind/reset", 1s, 1s},
      old_owner);
  old_owner->quiesce();
  auto gate = std::make_shared<std::atomic<bool>>(false);
  EXPECT_THROW(link.Stage(new_owner, new_owner, gate), std::invalid_argument);
  EXPECT_THROW(simulation.Stage(new_owner, new_owner, gate), std::invalid_argument);
  auto link_stage = link.Stage(old_owner, new_owner, gate);
  auto sim_stage = simulation.Stage(old_owner, new_owner, gate);
  ASSERT_TRUE(link.Validate(link_stage));
  ASSERT_TRUE(simulation.Validate(sim_stage));
  link.Commit(link_stage);
  simulation.Commit(sim_stage);
  EXPECT_FALSE(link.Validate(link_stage));
  EXPECT_FALSE(simulation.Validate(sim_stage));
  gate->store(true, std::memory_order_release);
  link_stage.reset();
  sim_stage.reset();
  new_owner->quiesce();
}
TEST(RosAdapterEntityOwner, AssistantHostLinkUsesOwnerAndStagesAgainstExactOwner) {
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  auto owner = ros2_control_execution::Ros2TransportResources::create(
      "assistant_owner_test", "", 1, [context] { return context; });
  auto new_owner = ros2_control_execution::Ros2TransportResources::create(
      "assistant_owner_new", "", 1, [context] { return context; });
  rclcpp::NodeOptions options;
  options.context(context).use_global_arguments(false);
  auto peer = std::make_shared<rclcpp::Node>("assistant_peer_test", options);
  auto health = peer->create_publisher<std_msgs::msg::Header>(
      "/assistant_owner/health", rclcpp::QoS(1).reliable());
  owner->start();
  new_owner->start();
  std::thread::id owner_thread;
  owner->invoke([&] { owner_thread = std::this_thread::get_id(); });

  auto events = std::make_shared<ClockEvents>();
  auto link = std::make_shared<RosAssistantHostLink>(
      owner->node(), "/assistant_owner", std::make_shared<TracedClock>(events), owner);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (health->get_subscription_count() == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_GT(health->get_subscription_count(), 0u);
  health->publish(std_msgs::msg::Header{});
  {
    std::unique_lock lock(events->mutex);
    ASSERT_TRUE(events->changed.wait_for(
        lock, 5s, [&] { return events->called != std::thread::id{}; }));
    EXPECT_EQ(events->called, owner_thread);
  }
  owner->invoke([] {});
  EXPECT_EQ(link->Observed().last_health_ns, 42);

  owner->quiesce();
  auto gate = std::make_shared<std::atomic<bool>>(false);
  EXPECT_THROW(link->Stage(new_owner, new_owner, gate), std::invalid_argument);
  auto stage = link->Stage(owner, new_owner, gate);
  ASSERT_TRUE(link->Validate(stage));
  link->Commit(stage);
  EXPECT_FALSE(link->Validate(stage));
  EXPECT_EQ(link->Observed().last_health_ns, 42);
  gate->store(true, std::memory_order_release);
  stage.reset();
  new_owner->quiesce();
  link.reset();
  owner.reset();
  new_owner.reset();
  health.reset();
  peer.reset();
  context->shutdown("assistant owner test complete");
}
} // namespace
} // namespace manyforge::roboplan_backend
