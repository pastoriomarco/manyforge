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
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_xarm_control/contract.hpp>

namespace manyforge::xarm_control
{

class FakeXArmHardwareBackend final : public IXArmHardwareBackend
{
public:
  FakeXArmHardwareBackend(
    std::string owner_id,
    std::size_t joint_count,
    std::int64_t observation_lease_ns);

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

  void set_transport_connected(FactState value);
  void set_identity_verified(FactState value);
  void set_position_valid(FactState value);
  void set_stationary(FactState value);
  void set_motion_enabled(FactState value);
  void set_vendor_control_ready(FactState value);
  void set_vendor_mode_and_state(int mode, int state);
  void set_controller_shutdown_eligible(FactState value);
  void set_fault_present(FactState value, int error_code = 1);
  void set_recovery_required(FactState value);
  void set_enable_motion_implies_vendor_ready(bool enabled);
  void set_auto_apply_commands(bool enabled);
  void set_next_return_code(int return_code);
  void apply_pending_effects();

  [[nodiscard]] std::vector<XArmLifecycleCommand> command_log() const;
  [[nodiscard]] std::size_t joint_write_count() const noexcept;
  [[nodiscard]] std::size_t command_gate_transition_count() const noexcept;
  [[nodiscard]] std::vector<double> last_joint_write() const;
  [[nodiscard]] std::vector<double> last_joint_velocity_write() const;

private:
  void bump_generation();
  void update_vendor_control_ready();
  void apply_effect(XArmLifecycleCommand command);

  std::string owner_id_;
  std::int64_t observation_lease_ns_;
  mutable XArmObservedState observation_;
  JointStateSample joint_state_;
  std::vector<XArmLifecycleCommand> command_log_;
  std::vector<XArmLifecycleCommand> pending_effects_;
  std::vector<double> last_joint_write_;
  std::vector<double> last_joint_velocity_write_;
  std::atomic<std::size_t> joint_write_count_{0};
  std::atomic<std::size_t> command_gate_transition_count_{0};
  std::atomic<bool> command_gate_open_{false};
  mutable std::mutex mutex_;
  bool enable_motion_implies_vendor_ready_ = true;
  bool auto_apply_commands_ = true;
  int next_return_code_ = 0;
};

std::vector<XArmActionPlan> make_fake_xarm_action_plans();

}  // namespace manyforge::xarm_control
