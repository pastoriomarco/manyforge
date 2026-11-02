#include "../src/simulated_plc_readiness.hpp"

#include <gtest/gtest.h>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_srvs/srv/set_bool.hpp>

#include <chrono>
#include <thread>
#include <unistd.h>

namespace manyforge::roboplan_backend::host_detail {
namespace {
using namespace std::chrono_literals;

// Names unique per process so parallel test runs on one graph never meet.
std::string unique_device() {
  return "simplc_probe_" + std::to_string(::getpid());
}

SimulatedPlcPlan plan_for(const std::string &device) {
  SimulatedPlcPlan plan;
  plan.device_id = device;
  plan.config_dir = "/unused";
  plan.subscribed_topics = {"/" + device + "/ready", "/" + device + "/out_0"};
  plan.served_services = {"/" + device + "/in_0"};
  return plan;
}

class SimulatedPlcReadinessTest : public ::testing::Test {
protected:
  void SetUp() override {
    context = std::make_shared<rclcpp::Context>();
    context->init(0, nullptr);
  }
  void TearDown() override { context->shutdown("test done"); }
  std::shared_ptr<rclcpp::Context> context;
};

TEST_F(SimulatedPlcReadinessTest, ServedServicesAndFirstSamplesMakeItReady) {
  const auto device = unique_device() + "_ready";
  const auto plan = plan_for(device);
  // A stand-in PLC in another Context (its own participant, like the process).
  auto peer_context = std::make_shared<rclcpp::Context>();
  peer_context->init(0, nullptr);
  auto peer = std::make_shared<rclcpp::Node>(
      "peer", rclcpp::NodeOptions().context(peer_context));
  std::vector<rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr> publishers;
  for (const auto &topic : plan.subscribed_topics)
    publishers.push_back(peer->create_publisher<std_msgs::msg::Bool>(topic, 10));
  auto service = peer->create_service<std_srvs::srv::SetBool>(
      plan.served_services.front(),
      [](const std::shared_ptr<std_srvs::srv::SetBool::Request>,
         std::shared_ptr<std_srvs::srv::SetBool::Response> response) {
        response->success = true;
      });
  auto timer = peer->create_wall_timer(100ms, [&publishers] {
    std_msgs::msg::Bool message;
    for (const auto &publisher : publishers) publisher->publish(message);
  });
  rclcpp::ExecutorOptions options;
  options.context = peer_context;
  rclcpp::executors::SingleThreadedExecutor executor(options);
  executor.add_node(peer);
  std::jthread spinner([&executor] { executor.spin(); });

  const auto readiness = WaitForSimulatedPlcEndpoints(context, plan, 5s);
  EXPECT_TRUE(readiness.ready());
  EXPECT_TRUE(readiness.missing_services.empty());
  EXPECT_TRUE(readiness.missing_topic_samples.empty());
  executor.cancel();
  spinner.join();
  peer_context->shutdown("test done");
}

TEST_F(SimulatedPlcReadinessTest, AbsentEndpointsAreNamedAfterTheBoundedWait) {
  const auto plan = plan_for(unique_device() + "_absent");
  const auto started = std::chrono::steady_clock::now();
  const auto readiness = WaitForSimulatedPlcEndpoints(context, plan, 300ms);
  EXPECT_GE(std::chrono::steady_clock::now() - started, 300ms);
  EXPECT_FALSE(readiness.ready());
  EXPECT_EQ(readiness.missing_services, plan.served_services);
  EXPECT_EQ(readiness.missing_topic_samples.size(), 2U);
}

TEST_F(SimulatedPlcReadinessTest, AnExitedProcessEndsTheWaitEarly) {
  const auto plan = plan_for(unique_device() + "_exited");
  const auto started = std::chrono::steady_clock::now();
  const auto readiness =
      WaitForSimulatedPlcEndpoints(context, plan, 5s, [] { return true; });
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
  EXPECT_FALSE(readiness.ready());
}

} // namespace
} // namespace manyforge::roboplan_backend::host_detail
