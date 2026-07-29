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

#include <manyforge_xarm_control/real_hardware_backend.hpp>

#include <stdexcept>
#include <utility>

namespace manyforge::xarm_control
{
RealXArmHardwareBackend::RealXArmHardwareBackend(
  RealXArmHardwareBackendConfig config,
  std::unique_ptr<IXArmSupervisedRosSession> transport)
: config_(std::move(config)), transport_(std::move(transport))
{
  if (config_.transport_owner_id.empty()) {
    throw std::invalid_argument("real xArm transport owner is empty");
  }
  if (transport_ == nullptr) {
    throw std::invalid_argument("real xArm transport session is missing");
  }
  if (transport_->owner_id() != config_.transport_owner_id) {
    throw std::invalid_argument("real xArm transport owner mismatch");
  }
}

std::string_view RealXArmHardwareBackend::owner_id() const noexcept
{
  return config_.transport_owner_id;
}

XArmObservedState RealXArmHardwareBackend::observe(
  std::int64_t now_ns) const
{
  auto result = transport_->observe_vendor(now_ns);
  observed_generation_.store(result.generation, std::memory_order_release);
  return result;
}

XArmLifecycleCommandResult
RealXArmHardwareBackend::execute_lifecycle(
  XArmLifecycleCommand command,
  std::string_view request_id,
  std::uint64_t expected_generation)
{
  return transport_->execute_primitive(
    command, request_id, expected_generation);
}

bool RealXArmHardwareBackend::read_joint_state(JointStateSample &)
{
  return false;
}

bool RealXArmHardwareBackend::write_joint_command(
  const JointCommandView &)
{
  return false;
}

XArmCommandGateUpdate RealXArmHardwareBackend::set_command_gate(
  bool open, std::int64_t valid_until_ns)
{
  const bool applied = transport_->set_command_gate(
    open, valid_until_ns,
    observed_generation_.load(std::memory_order_acquire));
  return {
    applied ? XArmCommandGateUpdateState::kApplied :
    XArmCommandGateUpdateState::kRejected,
    applied && open ? valid_until_ns : 0};
}

void RealXArmHardwareBackend::set_hardware_component_active(bool active)
{
  static_cast<void>(active);
}

void RealXArmHardwareBackend::set_trajectory_controller_active(bool active)
{
  static_cast<void>(active);
}

}  // namespace manyforge::xarm_control
