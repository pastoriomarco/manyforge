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
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_robot_lifecycle/supervisor.hpp>
#include <manyforge_xarm_control/contract.hpp>

namespace manyforge::xarm_control
{

class XArmLifecycleCoordinator final
{
public:
  XArmLifecycleCoordinator(
    robot_lifecycle::SupervisorConfig supervisor_config,
    std::unique_ptr<IXArmHardwareBackend> backend,
    std::vector<XArmActionPlan> action_plans,
    std::int64_t observation_lease_ns,
    std::size_t primitive_audit_capacity = 128,
    std::vector<XArmTargetPlan> target_plans = {});

  XArmLifecycleCoordinator(const XArmLifecycleCoordinator &) = delete;
  XArmLifecycleCoordinator & operator=(const XArmLifecycleCoordinator &) =
  delete;

  [[nodiscard]] std::string_view owner_id() const noexcept;

  bool refresh(std::int64_t now_ns);
  bool establish_authority_session(
    const robot_lifecycle::AuthoritySession & session,
    std::int64_t now_ns);
  bool clear_authority_session(
    std::string_view session_id,
    std::uint64_t session_generation,
    std::int64_t now_ns);
  robot_lifecycle::CommandDecision submit(
    const robot_lifecycle::CommandRequest & request,
    std::int64_t now_ns);
  bool reconcile(
    std::uint64_t transaction_generation,
    std::string_view request_id,
    std::string reason,
    std::int64_t now_ns);
  /// Apply one result returned by the production serialized lifecycle worker.
  ///
  /// Identity is checked against the exact deferred transaction primitive.
  /// A stale, duplicate, or replaced completion is ignored and can never
  /// mutate the current lifecycle transaction.
  bool complete_deferred_primitive(
    XArmLifecycleCommand command,
    std::string_view transport_request_id,
    std::uint64_t expected_observation_generation,
    const XArmLifecycleCommandResult & result,
    std::int64_t now_ns);

  void set_hardware_component_active(bool active);
  void set_trajectory_controller_active(bool active);
  bool read_joint_state(JointStateSample & sample);
  bool write_joint_command(
    const JointCommandView & command,
    std::int64_t now_ns);
  [[nodiscard]] bool motion_admitted(
    std::int64_t now_ns) const noexcept;

  [[nodiscard]] robot_lifecycle::LifecycleSnapshot snapshot(
    std::int64_t now_ns) const;
  [[nodiscard]] std::vector<XArmPrimitiveAuditRecord>
  primitive_audit_snapshot() const;

private:
  struct PrimitiveContext
  {
    std::uint64_t transaction_generation = 0;
    std::string request_id;
    std::string action_id;
    std::string stage_action_id;
    std::size_t step_index = 0;
    std::string transport_request_id;
    std::uint64_t expected_observation_generation = 0;
    XArmLifecycleCommand command = XArmLifecycleCommand::kClearError;
    bool target_stage = false;
    bool prior_step_attempted = false;
  };

  void require_controller_rearm();
  [[nodiscard]] const XArmActionPlan * find_plan(
    std::string_view action_id) const;
  [[nodiscard]] const XArmTargetPlan * find_target_plan(
    std::string_view action_id) const;
  [[nodiscard]] const XArmTargetStage * find_eligible_target_stage(
    const XArmTargetPlan & target,
    const XArmObservedState & observation) const;
  [[nodiscard]] robot_lifecycle::ObservedFacets map_observation(
    const XArmObservedState & observation,
    std::int64_t now_ns);
  [[nodiscard]] std::vector<robot_lifecycle::CapabilityDescriptor>
  map_capabilities(const XArmObservedState & observation) const;
  [[nodiscard]] bool plan_available(
    const XArmActionPlan & plan,
    const XArmObservedState & observation) const;
  [[nodiscard]] bool target_plan_available(
    const XArmTargetPlan & plan,
    const XArmObservedState & observation) const;
  [[nodiscard]] bool postcondition_satisfied(
    const XArmActionPlan & plan,
    const XArmObservedState & observation) const;
  [[nodiscard]] bool postcondition_satisfied(
    const XArmTargetPlan & plan,
    const XArmObservedState & observation) const;
  bool advance_target(
    const XArmTargetPlan & target,
    const XArmObservedState & observation,
    std::int64_t now_ns);
  bool dispatch_primitive(
    PrimitiveContext context,
    std::int64_t now_ns);
  bool apply_primitive_result(
    const PrimitiveContext & context,
    const XArmLifecycleCommandResult & result,
    std::int64_t now_ns);
  void clear_pending_context();
  void close_admission();
  void publish_admission_cache(std::int64_t now_ns);
  bool evaluate_pending(
    const XArmObservedState & observation,
    std::int64_t now_ns);
  void record_primitive(
    std::int64_t now_ns,
    std::uint64_t transaction_generation,
    std::string_view action_id,
    std::string_view stage_action_id,
    std::size_t step_index,
    std::string transport_request_id,
    std::uint64_t expected_observation_generation,
    XArmLifecycleCommand command,
    const XArmLifecycleCommandResult & result);

  robot_lifecycle::Supervisor supervisor_;
  std::unique_ptr<IXArmHardwareBackend> backend_;
  std::vector<XArmActionPlan> action_plans_;
  std::vector<XArmTargetPlan> target_plans_;
  std::int64_t observation_lease_ns_;
  std::uint64_t backend_observation_generation_ = 0;
  std::uint64_t observation_generation_ = 0;
  std::uint64_t capability_generation_ = 0;
  std::optional<std::uint64_t> pending_transaction_generation_;
  std::string pending_request_id_;
  std::string pending_action_id_;
  std::string pending_stage_action_id_;
  std::uint64_t pending_stage_observation_generation_ = 0;
  std::size_t pending_stage_sequence_ = 0;
  std::optional<PrimitiveContext> deferred_primitive_;
  FactState last_hardware_component_activity_fact_ = FactState::kUnknown;
  FactState last_controller_activity_fact_ = FactState::kUnknown;
  bool trajectory_controller_active_ = false;
  bool controller_rearm_required_ = true;
  bool controller_seen_inactive_since_fence_ = true;
  bool backend_command_gate_ready_ = false;
  bool backend_command_gate_known_closed_ = false;
  std::atomic<bool> motion_admitted_{false};
  std::atomic<std::int64_t> motion_admission_valid_until_ns_{0};
  std::size_t primitive_audit_capacity_;
  std::deque<XArmPrimitiveAuditRecord> primitive_audit_;
  std::uint64_t next_primitive_audit_sequence_ = 1;
};

}  // namespace manyforge::xarm_control
