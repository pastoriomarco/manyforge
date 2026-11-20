// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The vendor plugin's controller-manager calls answered by the execution host
// (`XArmPluginControllers`): the plugin's own flag machine calls twice per
// readiness edge and never while the readiness stays; ManyForge's own
// deactivation is one best-effort request to the real controller manager.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rclcpp/executors/multi_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>

#include <manyforge_xarm_control/xarm_plugin_controllers.hpp>

namespace manyforge::xarm_control {
namespace {

using namespace std::chrono_literals;
using ListControllers = controller_manager_msgs::srv::ListControllers;
using SwitchController = controller_manager_msgs::srv::SwitchController;

class RosEnvironment final : public ::testing::Environment {
public:
  void SetUp() override { rclcpp::init(0, nullptr); }
  void TearDown() override { rclcpp::shutdown(); }
};

[[maybe_unused]] auto *const kRosEnvironment =
    ::testing::AddGlobalTestEnvironment(new RosEnvironment);

[[nodiscard]] std::string unique_namespace() {
  std::random_device device;
  return "/xarm_plugin_controllers_test_" + std::to_string(device()) + "/manyforge/lite6";
}

/// The vendor plugin's controller switching (`UFRobotSystemHardware::write`,
/// `_deactivate_controller`, `_activate_controller`, `_call_request`), call
/// for call, on its own clients of the remapped names.
class FakePlugin final {
public:
  FakePlugin(const rclcpp::Node::SharedPtr &node, const std::string &stand_in)
      : list_(node->create_client<ListControllers>(stand_in + "/list_controllers")),
        switch_(node->create_client<SwitchController>(stand_in + "/switch_controller")) {}

  /// One control cycle's `write()` with the robot ready or not.
  void Write(const bool ready) {
    if (!ready) {
      Deactivate();
      return;
    }
    if (reactivate_controller_later_) {
      Activate();
      reactivate_controller_later_ = false;
    }
  }

  /// Calls sent, and calls answered within the plugin's 1 s bound.
  int calls{0};
  int answered{0};

private:
  void Deactivate() {
    if (reactivate_controller_later_) return;
    const auto listed = List();
    if (!listed || listed->controller.empty()) return;
    auto request = std::make_shared<SwitchController::Request>();
    bool valid_operation = false;
    for (const auto &controller : listed->controller) {
      if (controller.state == "active") valid_operation = true;
      request->deactivate_controllers.push_back(controller.name);
    }
    request->strictness = SwitchController::Request::BEST_EFFORT;
    if (valid_operation) {
      Call<SwitchController>(switch_, request);
      reactivate_controller_later_ = true;
    }
  }
  void Activate() {
    const auto listed = List();
    if (!listed || listed->controller.empty()) return;
    auto request = std::make_shared<SwitchController::Request>();
    for (const auto &controller : listed->controller)
      request->activate_controllers.push_back(controller.name);
    request->strictness = SwitchController::Request::BEST_EFFORT;
    Call<SwitchController>(switch_, request);
  }
  [[nodiscard]] ListControllers::Response::SharedPtr List() {
    return Call<ListControllers>(list_, std::make_shared<ListControllers::Request>());
  }
  template <typename Service>
  typename Service::Response::SharedPtr
  Call(const typename rclcpp::Client<Service>::SharedPtr &client,
       typename Service::Request::SharedPtr request) {
    ++calls;
    if (!client->wait_for_service(5s)) return nullptr;
    auto future = client->async_send_request(std::move(request));
    if (future.wait_for(1s) != std::future_status::ready) {
      client->remove_pending_request(future);
      return nullptr;
    }
    ++answered;
    return future.get();
  }

  rclcpp::Client<ListControllers>::SharedPtr list_;
  rclcpp::Client<SwitchController>::SharedPtr switch_;
  bool reactivate_controller_later_{false};
};

/// The machine's stand-in on the host node, the plugin's node and a fake
/// controller manager whose `switch_controller` answers when the test says.
struct World final {
  explicit World(std::shared_ptr<std::atomic<bool>> gate = nullptr)
      : ns(unique_namespace()), controller_manager(ns + "/controller_manager") {
    host_node = std::make_shared<rclcpp::Node>("xarm_plugin_controllers_host");
    plugin_node = std::make_shared<rclcpp::Node>("xarm_plugin_controllers_plugin");
    manager_node = std::make_shared<rclcpp::Node>("xarm_plugin_controllers_manager");
    if (gate) {
      owner = XArmRosOwner{host_node, {}, [](std::function<void()> work) { work(); },
                           [](std::shared_ptr<void> entity) { return entity; }, {}, {},
                           std::move(gate)};
    }
    stand_in = std::make_unique<XArmPluginControllers>(*host_node, ns + "/xarm",
                                                       controller_manager,
                                                       owner ? &*owner : nullptr);
    manager = manager_node->create_service<SwitchController>(
        controller_manager + "/switch_controller",
        [this](const std::shared_ptr<rclcpp::Service<SwitchController>> service,
               const std::shared_ptr<rmw_request_id_t> header,
               const SwitchController::Request::SharedPtr request) {
          std::lock_guard lock(mutex);
          requests.push_back(*request);
          pending.emplace_back(service, *header);
        });
    plugin = std::make_unique<FakePlugin>(plugin_node, ns + "/xarm/plugin_controllers");
    executor.add_node(host_node);
    executor.add_node(manager_node);
    executor.add_node(plugin_node);
    spinner = std::thread([this] { executor.spin(); });
  }

  ~World() {
    executor.cancel();
    if (spinner.joinable()) spinner.join();
  }

  /// The controller manager answers every request received so far.
  void AnswerAll() {
    std::lock_guard lock(mutex);
    for (auto &[service, header] : pending) {
      SwitchController::Response response;
      response.ok = true;
      service->send_response(header, response);
    }
    pending.clear();
  }
  [[nodiscard]] std::vector<SwitchController::Request> Requests() {
    std::lock_guard lock(mutex);
    return requests;
  }

  std::string ns;
  std::string controller_manager;
  rclcpp::Node::SharedPtr host_node;
  rclcpp::Node::SharedPtr plugin_node;
  rclcpp::Node::SharedPtr manager_node;
  std::optional<XArmRosOwner> owner;
  std::unique_ptr<XArmPluginControllers> stand_in;
  rclcpp::Service<SwitchController>::SharedPtr manager;
  std::unique_ptr<FakePlugin> plugin;
  std::mutex mutex;
  std::vector<SwitchController::Request> requests;
  std::vector<std::pair<std::shared_ptr<rclcpp::Service<SwitchController>>, rmw_request_id_t>>
      pending;
  rclcpp::executors::MultiThreadedExecutor executor{rclcpp::ExecutorOptions(), 4U};
  std::thread spinner;
};

TEST(XArmPluginControllers, ServesBelowTheDriverNamespace) {
  EXPECT_EQ(xarm_plugin_controllers_namespace("/manyforge/lite6/xarm"),
            "/manyforge/lite6/xarm/plugin_controllers");
  EXPECT_THROW(static_cast<void>(xarm_plugin_controllers_namespace("manyforge/lite6/xarm")),
               std::invalid_argument);
}

// The plugin's flag machine against the stand-in: one active entry listed,
// every switch answered without effect, so two calls (list, switch) per
// readiness edge and none while the readiness stays; nothing reaches the
// controller manager.
TEST(XArmPluginControllers, ThePluginCallsTwicePerReadinessEdgeAndNeverInBetween) {
  World world;
  for (int cycle = 0; cycle < 5; ++cycle) world.plugin->Write(true);
  EXPECT_EQ(world.plugin->calls, 0);
  // Lost readiness (an e-stop): list, then switch off, once.
  for (int cycle = 0; cycle < 5; ++cycle) world.plugin->Write(false);
  EXPECT_EQ(world.plugin->calls, 2);
  EXPECT_EQ(world.plugin->answered, 2);
  // Ready again: list, then switch on, once.
  for (int cycle = 0; cycle < 5; ++cycle) world.plugin->Write(true);
  EXPECT_EQ(world.plugin->calls, 4);
  EXPECT_EQ(world.plugin->answered, 4);
  // And again on the next loss.
  for (int cycle = 0; cycle < 5; ++cycle) world.plugin->Write(false);
  EXPECT_EQ(world.plugin->calls, 6);
  EXPECT_EQ(world.plugin->answered, 6);
  EXPECT_TRUE(world.Requests().empty());
}

// During an owner rebind only the active owner's servers answer.
TEST(XArmPluginControllers, AnInactiveOwnersServersDoNotAnswer) {
  auto gate = std::make_shared<std::atomic<bool>>(false);
  World world(gate);
  world.plugin->Write(false);
  EXPECT_EQ(world.plugin->calls, 1);
  EXPECT_EQ(world.plugin->answered, 0);
  gate->store(true, std::memory_order_release);
  world.plugin->Write(false);
  EXPECT_EQ(world.plugin->calls, 3);
  EXPECT_EQ(world.plugin->answered, 2);
}

// ManyForge's deactivation: one best-effort request deactivating exactly the
// named controllers, sent without waiting; `answered` runs only once the
// controller manager answers.
TEST(XArmPluginControllers, DeactivateIsOneBestEffortRequestAnsweredLater) {
  World world;
  ASSERT_TRUE(world.host_node
                  ->create_client<SwitchController>(world.controller_manager +
                                                    "/switch_controller")
                  ->wait_for_service(10s));
  // Its own client discovers the service on the same graph.
  const auto discovery = std::chrono::steady_clock::now() + 10s;
  std::promise<void> answered;
  auto answered_future = answered.get_future();
  bool sent = false;
  while (!sent && std::chrono::steady_clock::now() < discovery) {
    sent = world.stand_in->Deactivate({"lite6_traj_controller", "joint_state_broadcaster"},
                                      [&answered] { answered.set_value(); });
    if (!sent) std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(sent);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (world.Requests().empty() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  const auto requests = world.Requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests[0].deactivate_controllers,
            (std::vector<std::string>{"lite6_traj_controller", "joint_state_broadcaster"}));
  EXPECT_TRUE(requests[0].activate_controllers.empty());
  EXPECT_EQ(requests[0].strictness, SwitchController::Request::BEST_EFFORT);
  EXPECT_EQ(answered_future.wait_for(0s), std::future_status::timeout);
  world.AnswerAll();
  EXPECT_EQ(answered_future.wait_for(10s), std::future_status::ready);
}

// Without a discovered controller manager nothing is sent.
TEST(XArmPluginControllers, NothingIsSentToAnUndiscoveredControllerManager) {
  auto node = std::make_shared<rclcpp::Node>("xarm_plugin_controllers_alone");
  XArmPluginControllers alone(*node, unique_namespace() + "/xarm",
                              unique_namespace() + "/controller_manager");
  bool answered = false;
  EXPECT_FALSE(alone.Deactivate({"lite6_traj_controller"}, [&answered] { answered = true; }));
  EXPECT_FALSE(answered);
}

} // namespace
} // namespace manyforge::xarm_control
