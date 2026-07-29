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

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <manyforge_xarm_control/contract.hpp>

namespace manyforge::xarm_control
{

/// NRT client for the restricted ROS boundary owned by xarm_ros2.
///
/// The concrete session never owns or exposes an SDK object. It transports
/// named semantic operations to the one xarm_ros2 ros2_control process.
class IXArmSupervisedRosSession
{
public:
  virtual ~IXArmSupervisedRosSession() = default;

  IXArmSupervisedRosSession(
    const IXArmSupervisedRosSession &) = delete;
  IXArmSupervisedRosSession & operator=(
    const IXArmSupervisedRosSession &) = delete;

  [[nodiscard]] virtual std::string_view owner_id() const noexcept = 0;
  [[nodiscard]] virtual XArmObservedState observe_vendor(
    std::int64_t now_ns) const = 0;
  virtual XArmLifecycleCommandResult execute_primitive(
    XArmLifecycleCommand command,
    std::string_view request_id,
    std::uint64_t expected_generation) = 0;
  virtual bool set_command_gate(
    bool open,
    std::int64_t valid_until_ns,
    std::uint64_t expected_generation) = 0;

protected:
  IXArmSupervisedRosSession() = default;
};

struct RealXArmHardwareBackendConfig
{
  std::string transport_owner_id;
};

/// Production lifecycle backend around the restricted xarm_ros2 ROS surface.
///
/// Joint read/write ownership remains in the xarm_ros2 hardware plugin. This
/// backend only observes lifecycle state, overlays controller-manager facts,
/// and delegates one named operation at a time on an NRT path.
class RealXArmHardwareBackend final : public IXArmHardwareBackend
{
public:
  RealXArmHardwareBackend(
    RealXArmHardwareBackendConfig config,
    std::unique_ptr<IXArmSupervisedRosSession> transport);

  [[nodiscard]] std::string_view owner_id() const noexcept override;
  [[nodiscard]] XArmObservedState observe(
    std::int64_t now_ns) const override;
  XArmLifecycleCommandResult execute_lifecycle(
    XArmLifecycleCommand command,
    std::string_view request_id,
    std::uint64_t expected_generation) override;
  bool read_joint_state(JointStateSample & sample) override;
  bool write_joint_command(
    const JointCommandView & command) override;
  XArmCommandGateUpdate set_command_gate(
    bool open, std::int64_t valid_until_ns) override;
  void set_hardware_component_active(bool active) override;
  void set_trajectory_controller_active(bool active) override;

private:
  RealXArmHardwareBackendConfig config_;
  std::unique_ptr<IXArmSupervisedRosSession> transport_;
  mutable std::atomic<std::uint64_t> observed_generation_{0};
};

}  // namespace manyforge::xarm_control
