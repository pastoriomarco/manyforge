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

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_xarm_control/real_hardware_backend.hpp>

namespace manyforge::xarm_control
{
namespace
{

class RecordingTransport final : public IXArmSupervisedRosSession
{
public:
  explicit RecordingTransport(std::string owner)
  : owner_(std::move(owner))
  {
    observation_.generation = 7;
    observation_.transport_connected = FactState::kTrue;
    observation_.identity_verified = FactState::kTrue;
    observation_.position_valid = FactState::kTrue;
    observation_.hardware_component_active = FactState::kTrue;
    observation_.trajectory_controller_active = FactState::kTrue;
  }

  [[nodiscard]] std::string_view owner_id() const noexcept override
  {
    return owner_;
  }

  [[nodiscard]] XArmObservedState observe_vendor(
    std::int64_t) const override
  {
    ++observe_count;
    return observation_;
  }

  XArmLifecycleCommandResult execute_primitive(
    XArmLifecycleCommand command,
    std::string_view request_id,
    std::uint64_t expected_generation) override
  {
    commands.push_back(command);
    request_ids.emplace_back(request_id);
    expected_generations.push_back(expected_generation);
    XArmLifecycleCommandResult result;
    result.state = XArmLifecycleResultState::kResponseReceived;
    result.permitted = true;
    result.attempted = true;
    result.return_code = next_return_code;
    result.reason = "transport_result";
    result.observed_generation = observation_.generation;
    return result;
  }

  bool set_command_gate(
    bool open,
    std::int64_t valid_until_ns,
    std::uint64_t expected_generation) override
  {
    ++gate_update_count;
    gate_open = open && valid_until_ns > 0;
    gate_valid_until_ns = valid_until_ns;
    gate_expected_generation = expected_generation;
    return gate_open;
  }

  std::string owner_;
  mutable std::size_t observe_count = 0;
  std::size_t gate_update_count = 0;
  int next_return_code = 0;
  bool gate_open = false;
  std::int64_t gate_valid_until_ns = 0;
  std::uint64_t gate_expected_generation = 0;
  XArmObservedState observation_;
  std::vector<XArmLifecycleCommand> commands;
  std::vector<std::string> request_ids;
  std::vector<std::uint64_t> expected_generations;
};

struct BackendRig
{
  std::unique_ptr<RealXArmHardwareBackend> backend;
  RecordingTransport * transport = nullptr;
};

BackendRig make_backend()
{
  auto transport =
    std::make_unique<RecordingTransport>("lite6-a:sole-owner");
  auto * view = transport.get();
  RealXArmHardwareBackendConfig config;
  config.transport_owner_id = "lite6-a:sole-owner";
  auto backend = std::make_unique<RealXArmHardwareBackend>(
    std::move(config), std::move(transport));
  return {std::move(backend), view};
}

TEST(RealHardwareBackendTest, ConstructionAndActivityHaveNoTransportSideEffect)
{
  auto rig = make_backend();

  EXPECT_EQ(rig.transport->observe_count, 0U);
  EXPECT_TRUE(rig.transport->commands.empty());

  rig.backend->set_hardware_component_active(true);
  rig.backend->set_trajectory_controller_active(true);
  EXPECT_TRUE(rig.transport->commands.empty());
  const auto observation = rig.backend->observe(100);
  EXPECT_EQ(rig.transport->observe_count, 1U);
  EXPECT_EQ(observation.hardware_component_active, FactState::kTrue);
  EXPECT_EQ(observation.trajectory_controller_active, FactState::kTrue);
}

TEST(RealHardwareBackendTest, DelegatesExactlyOneApprovedPrimitive)
{
  auto rig = make_backend();
  rig.transport->next_return_code = -9;
  const auto command = XArmLifecycleCommand::kEnableMotion;

  const auto result = rig.backend->execute_lifecycle(
    command, "request-1", 7);

  ASSERT_EQ(rig.transport->commands.size(), 1U);
  EXPECT_EQ(rig.transport->commands.front(), command);
  EXPECT_EQ(rig.transport->request_ids.front(), "request-1");
  EXPECT_EQ(rig.transport->expected_generations.front(), 7U);
  EXPECT_EQ(result.return_code, -9);
  EXPECT_EQ(result.reason, "transport_result");
}

TEST(RealHardwareBackendTest, ProductionLifecycleBackendNeverTransfersJoints)
{
  auto rig = make_backend();
  JointStateSample sample;
  std::vector<double> positions(6, 0.2);
  std::vector<double> velocities(6, 0.05);
  EXPECT_FALSE(rig.backend->read_joint_state(sample));
  EXPECT_FALSE(rig.backend->write_joint_command(
      JointCommandView{positions, velocities}));
}

TEST(RealHardwareBackendTest, GateUsesLastObservedSourceGeneration)
{
  auto rig = make_backend();
  static_cast<void>(rig.backend->observe(100));
  const auto update = rig.backend->set_command_gate(true, 200);
  EXPECT_EQ(update.state, XArmCommandGateUpdateState::kApplied);
  EXPECT_EQ(update.effective_valid_until_ns, 200);
  EXPECT_EQ(rig.transport->gate_expected_generation, 7U);
}

TEST(RealHardwareBackendTest, RequiresExactSoleOwnerIdentity)
{
  auto transport = std::make_unique<RecordingTransport>("other-owner");
  RealXArmHardwareBackendConfig config;
  config.transport_owner_id = "lite6-a:sole-owner";
  EXPECT_THROW(
    RealXArmHardwareBackend(std::move(config), std::move(transport)),
    std::invalid_argument);
}

}  // namespace
}  // namespace manyforge::xarm_control
