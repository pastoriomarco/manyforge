// An accepted simulation state is the simulator's state at once.
#include <gtest/gtest.h>

#include "ros_simulation_control.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <rclcpp/executors/multi_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>
#include <thread>

namespace manyforge::roboplan_backend {
namespace {

using namespace std::chrono_literals;
using GetState = simulation_interfaces::srv::GetSimulationState;
using SetState = simulation_interfaces::srv::SetSimulationState;
using SimulationMessage = simulation_interfaces::msg::SimulationState;

class RosSimulationControlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    context = std::make_shared<rclcpp::Context>();
    context->init(0, nullptr);
    rclcpp::NodeOptions options;
    options.context(context);
    options.use_global_arguments(false);
    server = std::make_shared<rclcpp::Node>("manyforge_simulation_server_test", options);
    client = std::make_shared<rclcpp::Node>("manyforge_simulation_client_test", options);
    // The simulator still reports "stopped": only the accepted Set changes
    // what the control observes.
    get_state = server->create_service<GetState>(
        "/test_sim/get_state", [](const GetState::Request::SharedPtr,
                                  GetState::Response::SharedPtr response) {
          response->result.result = response->result.RESULT_OK;
          response->state.state = SimulationMessage::STATE_STOPPED;
        });
    set_state = server->create_service<SetState>(
        "/test_sim/set_state", [this](const SetState::Request::SharedPtr,
                                      SetState::Response::SharedPtr response) {
          response->result.result = answer;
        });
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context;
    executor = std::make_unique<rclcpp::executors::MultiThreadedExecutor>(executor_options, 2);
    executor->add_node(server);
    executor->add_node(client);
    spinner = std::thread([this] { executor->spin(); });
    // A poll period far beyond the test: no poll overwrites the observation.
    control = std::make_unique<RosSimulationControl>(
        client, SimulationServices{"/test_sim/get_state", "/test_sim/set_state",
                                   "/test_sim/reset", 2s, 1000s});
  }
  void TearDown() override {
    control.reset();
    executor->cancel();
    spinner.join();
    executor.reset();
    get_state.reset();
    set_state.reset();
    client.reset();
    server.reset();
    context->shutdown("simulation control fixture complete");
  }
  SimulationCall SetWhenReady(const SimulationState state) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    auto call = control->Set(state, {});
    while (call.error && call.error->code == "simulator_unreachable" &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(10ms);
      call = control->Set(state, {});
    }
    return call;
  }

  std::shared_ptr<rclcpp::Context> context;
  rclcpp::Node::SharedPtr server;
  rclcpp::Node::SharedPtr client;
  rclcpp::Service<GetState>::SharedPtr get_state;
  rclcpp::Service<SetState>::SharedPtr set_state;
  std::unique_ptr<rclcpp::executors::MultiThreadedExecutor> executor;
  std::thread spinner;
  std::unique_ptr<RosSimulationControl> control;
  std::uint8_t answer{simulation_interfaces::msg::Result::RESULT_OK};
};

TEST_F(RosSimulationControlTest, StateReflectsAnAcceptedSetImmediately) {
  ASSERT_FALSE(control->Observed().has_value());
  ASSERT_EQ(SetWhenReady(SimulationState::kPlaying).outcome, core::ActionOutcome::kSucceeded);
  EXPECT_EQ(control->Observed(), SimulationState::kPlaying);
  SimulatorComponentAdapter adapter(std::shared_ptr<ISimulationControl>(
      control.get(), [](ISimulationControl *) {}));
  EXPECT_EQ(adapter.State(0).lifecycle, core::ComponentLifecycle::kEnabled);
}

TEST_F(RosSimulationControlTest, AlreadyInTargetStateIsRecordedToo) {
  answer = SetState::Response::ALREADY_IN_TARGET_STATE;
  ASSERT_EQ(SetWhenReady(SimulationState::kPaused).outcome, core::ActionOutcome::kSucceeded);
  EXPECT_EQ(control->Observed(), SimulationState::kPaused);
}

TEST_F(RosSimulationControlTest, ARefusedSetChangesNothing) {
  answer = simulation_interfaces::msg::Result::RESULT_OPERATION_FAILED;
  EXPECT_EQ(SetWhenReady(SimulationState::kPlaying).outcome, core::ActionOutcome::kFailed);
  EXPECT_FALSE(control->Observed().has_value());
}

// The state poll: an answered poll records the simulator's state; an
// unanswered one is dropped after the response timeout (no usable state),
// and the next answered poll records the state again.
class RosSimulationControlPollTest : public ::testing::Test {
 protected:
  void SetUp() override {
    context = std::make_shared<rclcpp::Context>();
    context->init(0, nullptr);
    rclcpp::NodeOptions options;
    options.context(context);
    options.use_global_arguments(false);
    server = std::make_shared<rclcpp::Node>("manyforge_simulation_poll_server_test", options);
    client = std::make_shared<rclcpp::Node>("manyforge_simulation_poll_client_test", options);
    get_state = server->create_service<GetState>(
        "/test_poll_sim/get_state",
        [this](const std::shared_ptr<rclcpp::Service<GetState>> service,
               const std::shared_ptr<rmw_request_id_t> header,
               const GetState::Request::SharedPtr) {
          if (!answering.load()) return;  // never answered
          GetState::Response response;
          response.result.result = response.result.RESULT_OK;
          response.state.state = SimulationMessage::STATE_PAUSED;
          service->send_response(*header, response);
        });
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context;
    executor = std::make_unique<rclcpp::executors::MultiThreadedExecutor>(executor_options, 2);
    executor->add_node(server);
    executor->add_node(client);
    spinner = std::thread([this] { executor->spin(); });
    control = std::make_unique<RosSimulationControl>(
        client, SimulationServices{"/test_poll_sim/get_state", "/test_poll_sim/set_state",
                                   "/test_poll_sim/reset", 200ms, 20ms});
  }
  void TearDown() override {
    control.reset();
    executor->cancel();
    spinner.join();
    executor.reset();
    get_state.reset();
    client.reset();
    server.reset();
    context->shutdown("simulation poll fixture complete");
  }
  template <class Predicate> [[nodiscard]] static bool WaitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(10ms);
    }
    return true;
  }

  std::shared_ptr<rclcpp::Context> context;
  rclcpp::Node::SharedPtr server;
  rclcpp::Node::SharedPtr client;
  rclcpp::Service<GetState>::SharedPtr get_state;
  std::unique_ptr<rclcpp::executors::MultiThreadedExecutor> executor;
  std::thread spinner;
  std::unique_ptr<RosSimulationControl> control;
  std::atomic<bool> answering{true};
};

TEST_F(RosSimulationControlPollTest, PollRecordsStateAndDropsAnUnansweredRequest) {
  ASSERT_TRUE(WaitFor([&] { return control->Observed() == SimulationState::kPaused; }));
  answering = false;
  ASSERT_TRUE(WaitFor([&] { return !control->Observed().has_value(); }));
  answering = true;
  EXPECT_TRUE(WaitFor([&] { return control->Observed() == SimulationState::kPaused; }));
}

}  // namespace
}  // namespace manyforge::roboplan_backend
