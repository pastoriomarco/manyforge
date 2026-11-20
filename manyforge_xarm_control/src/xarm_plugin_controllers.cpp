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

#include <manyforge_xarm_control/xarm_plugin_controllers.hpp>

#include <atomic>
#include <chrono>
#include <utility>

#include <rclcpp/qos.hpp>

#include <manyforge_xarm_control/xarm_robot_states_transport.hpp>

namespace manyforge::xarm_control {

std::string xarm_plugin_controllers_namespace(const std::string_view driver_namespace) {
  // Validates the namespace the same way as the status topic.
  static_cast<void>(xarm_robot_states_topic(driver_namespace));
  return std::string(driver_namespace) + "/plugin_controllers";
}

XArmPluginControllers::XArmPluginControllers(rclcpp::Node &node,
                                             const std::string_view driver_namespace,
                                             const std::string &controller_manager,
                                             const XArmRosOwner *ros_owner)
    : logger_(node.get_logger()) {
  const auto ns = xarm_plugin_controllers_namespace(driver_namespace);
  const auto create = [&] {
    // Two bundles: the stand-in servers run in their own time-critical group
    // (OD-66; the plugin's control loop waits on them) and retire with it;
    // the deactivation client stays on the owner's default group, with the
    // report callback that sends on it.
    struct StandIn final {
      rclcpp::Node::SharedPtr node;
      rclcpp::CallbackGroup::SharedPtr group;
      rclcpp::Service<ListControllers>::SharedPtr list_controllers;
      rclcpp::Service<SwitchController>::SharedPtr switch_controller;
    };
    struct Client final {
      rclcpp::Node::SharedPtr node;
      rclcpp::Client<SwitchController>::SharedPtr deactivate;
    };
    auto stand_in = std::make_shared<StandIn>();
    auto client = std::make_shared<Client>();
    if (ros_owner) {
      stand_in->node = client->node = ros_owner->node;
      if (ros_owner->time_critical_group) stand_in->group = ros_owner->time_critical_group();
    }
    const auto active = ros_owner ? ros_owner->active : nullptr;
    // The callbacks only fill the response; an inactive owner's server
    // doesn't answer (the active one does).
    stand_in->list_controllers = node.create_service<ListControllers>(
        ns + "/list_controllers",
        [active](const std::shared_ptr<rclcpp::Service<ListControllers>> service,
                 const std::shared_ptr<rmw_request_id_t> header,
                 const ListControllers::Request::SharedPtr) {
          if (active && !active->load(std::memory_order_acquire)) return;
          ListControllers::Response response;
          controller_manager_msgs::msg::ControllerState listed;
          listed.name = std::string(kXArmPluginListedController);
          listed.state = "active";
          response.controller.push_back(std::move(listed));
          service->send_response(*header, response);
        },
        rclcpp::ServicesQoS(), stand_in->group);
    stand_in->switch_controller = node.create_service<SwitchController>(
        ns + "/switch_controller",
        [active](const std::shared_ptr<rclcpp::Service<SwitchController>> service,
                 const std::shared_ptr<rmw_request_id_t> header,
                 const SwitchController::Request::SharedPtr) {
          if (active && !active->load(std::memory_order_acquire)) return;
          SwitchController::Response response;
          response.ok = true;
          service->send_response(*header, response);
        },
        rclcpp::ServicesQoS(), stand_in->group);
    client->deactivate =
        node.create_client<SwitchController>(controller_manager + "/switch_controller");
    std::shared_ptr<void> stand_in_held = stand_in;
    if (ros_owner)
      stand_in_held = stand_in->group && ros_owner->retain_time_critical
                          ? ros_owner->retain_time_critical(std::move(stand_in_held),
                                                            stand_in->group)
                          : ros_owner->retain(std::move(stand_in_held));
    auto *const servers = static_cast<StandIn *>(stand_in_held.get());
    list_controllers_ = decltype(list_controllers_)(stand_in_held, servers->list_controllers.get());
    switch_controller_ =
        decltype(switch_controller_)(stand_in_held, servers->switch_controller.get());
    std::shared_ptr<void> client_held = ros_owner ? ros_owner->retain(std::move(client)) : client;
    deactivate_ = decltype(deactivate_)(client_held,
                                        static_cast<Client *>(client_held.get())->deactivate.get());
  };
  if (ros_owner) ros_owner->invoke(create);
  else create();
}

bool XArmPluginControllers::Deactivate(std::vector<std::string> controllers,
                                       std::function<void()> answered) const {
  rclcpp::Client<SwitchController>::SharedPtr client;
  {
    std::lock_guard lock(mutex_);
    client = deactivate_;
  }
  // No discovery wait: an undiscovered service is not sent.
  if (!client->service_is_ready()) return false;
  auto request = std::make_shared<SwitchController::Request>();
  request->deactivate_controllers = std::move(controllers);
  request->strictness = SwitchController::Request::BEST_EFFORT;
  request->activate_asap = false;
  const auto started = std::chrono::steady_clock::now();
  static_cast<void>(client->async_send_request(
      std::move(request),
      [logger = logger_, started, answered = std::move(answered)](
          rclcpp::Client<SwitchController>::SharedFuture future) {
        const auto response = future.get();
        RCLCPP_INFO(logger,
                    "manyforge_component_call call=xarm.deactivate_controllers outcome=%s "
                    "duration_ms=%.1f",
                    response && response->ok ? "answered" : "refused",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              started)
                        .count());
        answered();
      }));
  return true;
}

void XArmPluginControllers::SwapRosState(XArmPluginControllers &replacement) noexcept {
  std::scoped_lock lock(mutex_, replacement.mutex_);
  list_controllers_.swap(replacement.list_controllers_);
  switch_controller_.swap(replacement.switch_controller_);
  deactivate_.swap(replacement.deactivate_);
}

} // namespace manyforge::xarm_control
