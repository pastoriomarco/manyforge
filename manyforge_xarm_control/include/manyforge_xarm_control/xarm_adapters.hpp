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

#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

#include <rclcpp/node.hpp>

#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_xarm_control/xarm_driver_services.hpp>
#include <manyforge_xarm_control/xarm_io_states_transport.hpp>
#include <manyforge_xarm_control/xarm_launch_contribution.hpp>
#include <manyforge_xarm_control/xarm_owner_adapter.hpp>
#include <manyforge_xarm_control/xarm_robot_states_transport.hpp>

namespace manyforge::xarm_control {

/// The adapter family name in the `manyforge_component_adapters` resource
/// index (CONTRACTS §6) and in `lanes.real.adapter`.
inline constexpr std::string_view kXArmAdapterFamily = "xarm";

/// One xArm machine's connection: the execution host's pieces its owner runs
/// on.
struct XArmMachineInputs final {
  /// `<component namespace>/xarm`.
  std::string driver_namespace;
  /// The machine's address (site settings), probed during a reboot.
  std::string address;
  XArmVendorSettings settings;
  /// The ros2_control lane adapter of the machine's connection.
  std::shared_ptr<core::IComponentAdapter> lane;
  std::function<core::Result<void>(std::stop_token)> restart_driver;
  XArmIoUpdateSink io_sink;
  /// The machine's compiled lifecycle command time (its owner's `prepare`).
  std::chrono::nanoseconds lifecycle_command_timeout{};
  /// Whether the machine's launch is still starting (owner port of the same
  /// name).
  std::function<bool()> launch_settling;
};

/// One machine (CONTRACTS §16): the owner adapter with the driver services
/// and the subscriptions it holds; its members register with `owner`.
struct XArmMachine final {
  std::shared_ptr<XArmRobotStatesTransport> robot_states;
  std::shared_ptr<XArmDriverServices> driver;
  std::shared_ptr<XArmOwnerAdapter> owner;
  std::shared_ptr<XArmIoStatesTransport> io_states;
};

/// Creates the subscriptions and service clients on `node` (stamping receipt
/// on `clock`); both outlive the machine.
[[nodiscard]] XArmMachine make_xarm_machine(rclcpp::Node &node, core::IClock &clock,
                                            XArmMachineInputs inputs);

/// One probe of the controller's command port (TCP 502), bounded by
/// `timeout` (`reboot.probe_interval_ms`): whether it accepts a connection now.
[[nodiscard]] bool xarm_controller_reachable(const std::string &address,
                                             std::chrono::nanoseconds timeout);

} // namespace manyforge::xarm_control
