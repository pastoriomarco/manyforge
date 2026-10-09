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

#ifndef MANYFORGE_MOTION_CONTROLLER__MOTION_CONTROLLER_HPP_
#define MANYFORGE_MOTION_CONTROLLER__MOTION_CONTROLLER_HPP_

#include <memory>
#include <string>
#include <vector>

#include "controller_interface/controller_interface.hpp"
#include "rclcpp/duration.hpp"
#include "rclcpp/time.hpp"
#include "rclcpp_lifecycle/state.hpp"

namespace manyforge_motion_controller
{

class ControllerMechanism;
class MotionControllerTestPeer;

// The controller-loop integration point. Motion remains unavailable until the
// canonical shared step and its command-realization boundary are integrated.
class MotionController final : public controller_interface::ControllerInterface
{
public:
  MotionController();
  ~MotionController() override;
  controller_interface::CallbackReturn on_init() override;
  controller_interface::CallbackReturn on_configure(
    const rclcpp_lifecycle::State & previous_state) override;
  controller_interface::CallbackReturn on_activate(
    const rclcpp_lifecycle::State & previous_state) override;
  controller_interface::CallbackReturn on_cleanup(
    const rclcpp_lifecycle::State & previous_state) override;
  controller_interface::CallbackReturn on_shutdown(
    const rclcpp_lifecycle::State & previous_state) override;
  controller_interface::CallbackReturn on_error(
    const rclcpp_lifecycle::State & previous_state) override;

  controller_interface::InterfaceConfiguration command_interface_configuration() const override;
  controller_interface::InterfaceConfiguration state_interface_configuration() const override;
  controller_interface::return_type update(
    const rclcpp::Time & time, const rclcpp::Duration & period) override;

private:
  friend class MotionControllerTestPeer;
  void clear_configuration();

  // Constructed only when a future host supplies a qualified receiver profile.
  std::unique_ptr<ControllerMechanism> mechanism_;

  // Ordered lifecycle resource claims.
  std::vector<std::string> command_names_;
  std::vector<std::string> state_names_;
};

}  // namespace manyforge_motion_controller

#endif  // MANYFORGE_MOTION_CONTROLLER__MOTION_CONTROLLER_HPP_
