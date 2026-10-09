// Copyright 2026 Flexin Group SRL
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

#ifndef MANYFORGE_ISAAC_HARDWARE__ISAAC_SYSTEM_HPP_
#define MANYFORGE_ISAAC_HARDWARE__ISAAC_SYSTEM_HPP_

#include <memory>
#include <vector>

#include "hardware_interface/system_interface.hpp"

namespace manyforge_isaac_hardware
{

// Bounded six-joint observation transport. Activation is unavailable until the
// binding/lease command path is supplied by the execution contract owner.
class IsaacSystem final : public hardware_interface::SystemInterface
{
public:
  IsaacSystem();
  ~IsaacSystem() override;

  hardware_interface::CallbackReturn on_init(
    const hardware_interface::HardwareComponentInterfaceParams & params) override;
  std::vector<hardware_interface::StateInterface::ConstSharedPtr>
  on_export_state_interfaces() override;
  std::vector<hardware_interface::CommandInterface::SharedPtr>
  on_export_command_interfaces() override;

  hardware_interface::CallbackReturn on_configure(const rclcpp_lifecycle::State &) override;
  hardware_interface::CallbackReturn on_cleanup(const rclcpp_lifecycle::State &) override;
  hardware_interface::CallbackReturn on_activate(const rclcpp_lifecycle::State &) override;
  hardware_interface::CallbackReturn on_deactivate(const rclcpp_lifecycle::State &) override;
  hardware_interface::CallbackReturn on_shutdown(const rclcpp_lifecycle::State &) override;
  hardware_interface::CallbackReturn on_error(const rclcpp_lifecycle::State &) override;

  hardware_interface::return_type read(
    const rclcpp::Time &, const rclcpp::Duration &) override;
  hardware_interface::return_type write(
    const rclcpp::Time &, const rclcpp::Duration &) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge_isaac_hardware

#endif  // MANYFORGE_ISAAC_HARDWARE__ISAAC_SYSTEM_HPP_
