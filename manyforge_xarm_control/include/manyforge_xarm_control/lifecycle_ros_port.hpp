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

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <manyforge_robot_lifecycle/control_port.hpp>
#include <manyforge_xarm_control/coordinator.hpp>
#include <manyforge_xarm_control/xarm_ros_transport_session.hpp>

namespace manyforge::xarm_control
{

class SerializedXArmHardwareBackend;

struct XArmLifecycleRosPortConfig
{
  std::string robot_id;
  std::string supervisor_instance_id;
  robot_lifecycle::AuthorityProfile authority_profile =
    robot_lifecycle::AuthorityProfile::kLocalOperator;
  XArmRosTransportSessionConfig transport;
  std::int64_t observation_lease_ns = 250'000'000;
};

/// Internal production port used by a trusted authority adapter.
///
/// This class exposes no ROS, HTTP, or Composer mutation endpoint. It joins
/// the generic lifecycle reducer to the restricted xarm_ros2 ROS client so a
/// later authenticated local-operator or PLC adapter can share one contract.
class XArmLifecycleRosPort final
  : public robot_lifecycle::LifecycleControlPort
{
public:
  explicit XArmLifecycleRosPort(XArmLifecycleRosPortConfig config);
  ~XArmLifecycleRosPort() override;

  bool establish_authority_session(
    const robot_lifecycle::AuthoritySession & session,
    std::int64_t now_ns) override;
  bool clear_authority_session(
    std::string_view session_id,
    std::uint64_t session_generation,
    std::int64_t now_ns) override;
  robot_lifecycle::CommandDecision submit_lifecycle(
    const robot_lifecycle::CommandRequest & request,
    std::int64_t now_ns) override;
  bool reconcile_lifecycle(
    std::uint64_t transaction_generation,
    std::string_view request_id,
    std::string reason,
    std::int64_t now_ns) override;
  bool refresh_lifecycle(std::int64_t now_ns) override;
  [[nodiscard]] robot_lifecycle::LifecycleSnapshot lifecycle_snapshot(
    std::int64_t now_ns) const override;
  [[nodiscard]] std::vector<XArmPrimitiveAuditRecord>
  primitive_audit_snapshot() const;

private:
  void lifecycle_worker();
  void command_gate_worker();

  mutable std::mutex mutex_;
  std::unique_ptr<XArmLifecycleCoordinator> coordinator_;
  SerializedXArmHardwareBackend * serialized_backend_ = nullptr;
  std::thread lifecycle_worker_;
  std::thread command_gate_worker_;
};

}  // namespace manyforge::xarm_control
