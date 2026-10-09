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

#include "manyforge_motion_controller/motion_controller.hpp"

#include <exception>
#include <string>
#include <unordered_set>
#include <vector>

#include "controller_mechanism.hpp"
#include "hardware_interface/types/hardware_interface_type_values.hpp"
#include "pluginlib/class_list_macros.hpp"
#include "rclcpp/logging.hpp"

namespace manyforge_motion_controller
{

MotionController::MotionController() = default;
MotionController::~MotionController() = default;

controller_interface::CallbackReturn MotionController::on_init()
{
  try {
    auto_declare<std::vector<std::string>>("joints", {});
  } catch (const std::exception & error) {
    RCLCPP_ERROR(get_node()->get_logger(), "Controller initialization failed: %s", error.what());
    return controller_interface::CallbackReturn::ERROR;
  }
  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn MotionController::on_configure(
  const rclcpp_lifecycle::State &)
{
  clear_configuration();
  try {
    const auto joints = get_node()->get_parameter("joints").as_string_array();
    if (joints.empty() || joints.size() > core::kMaxMotionJoints) {
      RCLCPP_ERROR(get_node()->get_logger(),
          "The joints parameter must contain one to sixteen resources");
      return controller_interface::CallbackReturn::FAILURE;
    }
    std::unordered_set<std::string> unique_joints;
    for (const auto & joint : joints) {
      if (joint.empty() || !unique_joints.insert(joint).second) {
        RCLCPP_ERROR(get_node()->get_logger(), "Joint resource names must be nonempty and unique");
        return controller_interface::CallbackReturn::FAILURE;
      }
    }
    command_names_.reserve(joints.size());
    state_names_.reserve(2U * joints.size());
    for (const auto & joint : joints) {
      command_names_.push_back(joint + "/" + hardware_interface::HW_IF_POSITION);
      state_names_.push_back(joint + "/" + hardware_interface::HW_IF_POSITION);
      state_names_.push_back(joint + "/" + hardware_interface::HW_IF_VELOCITY);
    }
  } catch (const std::exception & error) {
    clear_configuration();
    RCLCPP_ERROR(get_node()->get_logger(), "Controller configuration failed: %s", error.what());
    return controller_interface::CallbackReturn::FAILURE;
  }
  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn MotionController::on_activate(
  const rclcpp_lifecycle::State &)
{
  RCLCPP_ERROR(
    get_node()->get_logger(),
    "Motion unavailable: host wiring, independent watchdog and qualified delivery "
    "boundary are not integrated");
  return controller_interface::CallbackReturn::FAILURE;
}

controller_interface::CallbackReturn MotionController::on_cleanup(
  const rclcpp_lifecycle::State &)
{
  clear_configuration();
  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn MotionController::on_shutdown(
  const rclcpp_lifecycle::State &)
{
  clear_configuration();
  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::CallbackReturn MotionController::on_error(
  const rclcpp_lifecycle::State &)
{
  clear_configuration();
  return controller_interface::CallbackReturn::SUCCESS;
}

controller_interface::InterfaceConfiguration
MotionController::command_interface_configuration() const
{
  return {controller_interface::interface_configuration_type::INDIVIDUAL, command_names_};
}

controller_interface::InterfaceConfiguration MotionController::state_interface_configuration() const
{
  return {controller_interface::interface_configuration_type::INDIVIDUAL, state_names_};
}

controller_interface::return_type MotionController::update(
  const rclcpp::Time &, const rclcpp::Duration &)
{
  if (!mechanism_) {
    return controller_interface::return_type::ERROR;
  }
  return mechanism_->Tick(mechanism_->LocalNow(), command_interfaces_) ?
         controller_interface::return_type::OK : controller_interface::return_type::ERROR;
}

void MotionController::clear_configuration()
{
  // Lifecycle teardown requires the manager to have quiesced update first.
  mechanism_.reset();
  command_names_.clear();
  state_names_.clear();
}

}  // namespace manyforge_motion_controller

PLUGINLIB_EXPORT_CLASS(
  manyforge_motion_controller::MotionController, controller_interface::ControllerInterface)
