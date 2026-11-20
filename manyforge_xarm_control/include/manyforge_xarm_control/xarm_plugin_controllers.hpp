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

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <controller_manager_msgs/srv/list_controllers.hpp>
#include <controller_manager_msgs/srv/switch_controller.hpp>
#include <rclcpp/client.hpp>
#include <rclcpp/node.hpp>
#include <rclcpp/service.hpp>

#include <manyforge_xarm_control/xarm_ros_owner.hpp>

namespace manyforge::xarm_control {

/// Where the vendor plugin's controller-manager calls are answered:
/// `<driver namespace>/plugin_controllers` (`vendor/manyforge_settings.yaml`
/// remaps its `/controller_manager/list_controllers` and
/// `/controller_manager/switch_controller` there).
[[nodiscard]] std::string xarm_plugin_controllers_namespace(std::string_view driver_namespace);

/// The one controller the stand-in lists, always `active`. Any name: the
/// plugin only sends it back in its `switch_controller`.
inline constexpr std::string_view kXArmPluginListedController = "manyforge_arm_controllers";

/// The vendor plugin's controller switching, taken over by ManyForge.
///
/// The plugin (`UFRobotSystemHardware::write`) switches every active
/// controller off while its robot is not ready and all back on once ready,
/// through blocking `list_controllers` and `switch_controller` calls from the
/// control loop; against the real controller manager its switch waits on
/// that same loop (up to its 1 s timeout per call) and races ManyForge's own
/// switches. Its calls are answered here instead, in the execution host:
/// `list_controllers` with one active entry (so it calls only at a
/// readiness edge, twice), `switch_controller` with `ok` and no effect. The
/// callbacks only fill the response, on a time-critical group of the ROS
/// owner; during an owner rebind only the active owner's servers answer.
///
/// ManyForge switches the arm's controllers itself: off on lost readiness
/// (`Deactivate`, from the arm adapter's report callback), on only at Enable.
class XArmPluginControllers final {
public:
  /// `controller_manager`: the component's controller manager node
  /// (`<component namespace>/controller_manager`).
  XArmPluginControllers(rclcpp::Node &node, std::string_view driver_namespace,
                        const std::string &controller_manager,
                        const XArmRosOwner *ros_owner = nullptr);

  /// One best-effort `switch_controller` to the controller manager
  /// deactivating `controllers`, sent without waiting; `answered` runs on the
  /// node's executor once the controller manager answers. Not sent (false)
  /// while the service is not discovered.
  [[nodiscard]] bool Deactivate(std::vector<std::string> controllers,
                                std::function<void()> answered) const;

  /// Preserve this object's identity while rebinding the servers and the
  /// client.
  void SwapRosState(XArmPluginControllers &replacement) noexcept;

private:
  using ListControllers = controller_manager_msgs::srv::ListControllers;
  using SwitchController = controller_manager_msgs::srv::SwitchController;

  mutable std::mutex mutex_;
  rclcpp::Logger logger_;
  rclcpp::Service<ListControllers>::SharedPtr list_controllers_;
  rclcpp::Service<SwitchController>::SharedPtr switch_controller_;
  rclcpp::Client<SwitchController>::SharedPtr deactivate_;
};

} // namespace manyforge::xarm_control
