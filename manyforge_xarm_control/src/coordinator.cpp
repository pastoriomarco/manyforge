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

#include <manyforge_xarm_control/coordinator.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace manyforge::xarm_control
{
namespace
{

bool is_true(FactState state)
{
  return state == FactState::kTrue;
}

bool is_false(FactState state)
{
  return state == FactState::kFalse;
}

std::int64_t saturating_add(
  std::int64_t value, std::int64_t positive_delta)
{
  if (value >
    std::numeric_limits<std::int64_t>::max() - positive_delta)
  {
    return std::numeric_limits<std::int64_t>::max();
  }
  return value + positive_delta;
}

bool valid_condition(const XArmStateCondition & condition)
{
  switch (condition.kind) {
    case XArmConditionKind::kModeEquals:
    case XArmConditionKind::kStateEquals:
      return condition.expected_value >= 0 &&
             condition.expected_value <= 255;
    default:
      return condition.expected_value == 0 ||
             condition.expected_value == 1;
  }
}

void validate_plan(const XArmActionPlan & plan)
{
  if (plan.action_id.empty() || plan.label.empty() ||
    plan.side_effect_summary.empty() ||
    plan.expected_postcondition.empty() ||
    plan.timeout_ns <= 0 || plan.preconditions.empty() ||
    plan.steps.empty() || plan.postconditions.empty())
  {
    throw std::invalid_argument("xArm action plan is incomplete");
  }
  if (!std::all_of(
      plan.preconditions.begin(), plan.preconditions.end(),
      valid_condition) ||
    !std::all_of(
      plan.postconditions.begin(), plan.postconditions.end(),
      valid_condition))
  {
    throw std::invalid_argument("xArm action plan condition is invalid");
  }
  if (plan.terminal_on_known_success &&
    (plan.steps.size() != 1 ||
    plan.steps.front() != XArmLifecycleCommand::kShutdownController))
  {
    throw std::invalid_argument(
            "terminal-on-success is reserved for controller shutdown");
  }
}

void validate_target_plan(const XArmTargetPlan & plan)
{
  if (plan.action_id.empty() || plan.label.empty() ||
    plan.side_effect_summary.empty() ||
    plan.expected_postcondition.empty() ||
    plan.timeout_ns <= 0 || plan.preconditions.empty() ||
    plan.stages.empty() || plan.postconditions.empty())
  {
    throw std::invalid_argument("xArm target plan is incomplete");
  }
  if (!std::all_of(
      plan.preconditions.begin(), plan.preconditions.end(),
      valid_condition) ||
    !std::all_of(
      plan.postconditions.begin(), plan.postconditions.end(),
      valid_condition))
  {
    throw std::invalid_argument("xArm target plan condition is invalid");
  }
  std::unordered_set<std::string> stage_ids;
  for (const auto & stage : plan.stages) {
    if (stage.action_id.empty() || stage.eligibility.empty() ||
      !stage_ids.insert(stage.action_id).second ||
      !std::all_of(
        stage.eligibility.begin(), stage.eligibility.end(),
        valid_condition))
    {
      throw std::invalid_argument("xArm target stage is invalid");
    }
  }
}

bool fact_matches(FactState value, int expected_value)
{
  return expected_value == 1 ? is_true(value) : is_false(value);
}

bool condition_satisfied(
  const XArmStateCondition & condition,
  const XArmObservedState & observation)
{
  switch (condition.kind) {
    case XArmConditionKind::kTransportConnected:
      return fact_matches(
        observation.transport_connected, condition.expected_value);
    case XArmConditionKind::kIdentityVerified:
      return fact_matches(
        observation.identity_verified, condition.expected_value);
    case XArmConditionKind::kPositionValid:
      return fact_matches(
        observation.position_valid, condition.expected_value);
    case XArmConditionKind::kMotionEnabled:
      return fact_matches(
        observation.motion_enabled, condition.expected_value);
    case XArmConditionKind::kVendorControlReady:
      return fact_matches(
        observation.vendor_control_ready, condition.expected_value);
    case XArmConditionKind::kHardwareComponentActive:
      return fact_matches(
        observation.hardware_component_active, condition.expected_value);
    case XArmConditionKind::kTrajectoryControllerActive:
      return fact_matches(
        observation.trajectory_controller_active,
        condition.expected_value);
    case XArmConditionKind::kFaultPresent:
      return fact_matches(
        observation.fault_present, condition.expected_value);
    case XArmConditionKind::kRecoveryRequired:
      return fact_matches(
        observation.recovery_required, condition.expected_value);
    case XArmConditionKind::kWarningPresent:
      return condition.expected_value == 1 ?
             observation.warning_code > 0 :
             observation.warning_code == 0;
    case XArmConditionKind::kStationary:
      return fact_matches(
        observation.stationary, condition.expected_value);
    case XArmConditionKind::kControllerShutdownEligible:
      return fact_matches(
        observation.controller_shutdown_eligible,
        condition.expected_value);
    case XArmConditionKind::kModeEquals:
      return observation.mode == condition.expected_value;
    case XArmConditionKind::kStateEquals:
      return observation.state == condition.expected_value;
    case XArmConditionKind::kReadyStateRequestEligible:
      return fact_matches(
        observation.state == 4 || observation.state == 5 ?
        FactState::kTrue : FactState::kFalse,
        condition.expected_value);
  }
  return false;
}

}  // namespace

XArmLifecycleCoordinator::XArmLifecycleCoordinator(
  robot_lifecycle::SupervisorConfig supervisor_config,
  std::unique_ptr<IXArmHardwareBackend> backend,
  std::vector<XArmActionPlan> action_plans,
  std::int64_t observation_lease_ns,
  std::size_t primitive_audit_capacity,
  std::vector<XArmTargetPlan> target_plans)
: supervisor_(std::move(supervisor_config)),
  backend_(std::move(backend)),
  action_plans_(std::move(action_plans)),
  target_plans_(std::move(target_plans)),
  observation_lease_ns_(observation_lease_ns),
  primitive_audit_capacity_(primitive_audit_capacity)
{
  if (backend_ == nullptr) {
    throw std::invalid_argument("xArm hardware backend is required");
  }
  if (backend_->owner_id().empty()) {
    throw std::invalid_argument("xArm hardware owner_id is required");
  }
  if (observation_lease_ns_ <= 0) {
    throw std::invalid_argument("observation lease must be positive");
  }
  if (primitive_audit_capacity_ == 0) {
    throw std::invalid_argument(
            "primitive audit capacity must be positive");
  }
  std::unordered_set<std::string> action_ids;
  for (const auto & plan : action_plans_) {
    validate_plan(plan);
    if (!action_ids.insert(plan.action_id).second) {
      throw std::invalid_argument("duplicate xArm action_id");
    }
  }
  for (const auto & target : target_plans_) {
    validate_target_plan(target);
    if (!action_ids.insert(target.action_id).second) {
      throw std::invalid_argument("duplicate xArm action_id");
    }
    for (const auto & stage : target.stages) {
      const XArmActionPlan * primitive = find_plan(stage.action_id);
      if (primitive == nullptr || primitive->steps.size() != 1 ||
        primitive->terminal_on_known_success)
      {
        throw std::invalid_argument(
                "xArm target stage must reference one ordinary primitive");
      }
    }
  }
  close_admission();
}

std::string_view XArmLifecycleCoordinator::owner_id() const noexcept
{
  return backend_->owner_id();
}

bool XArmLifecycleCoordinator::refresh(std::int64_t now_ns)
{
  const bool admission_was_open =
    motion_admitted_.load(std::memory_order_acquire);
  supervisor_.advance_time(now_ns);
  const XArmObservedState observation = backend_->observe(now_ns);
  const bool hardware_activity_changed =
    observation.hardware_component_active !=
    last_hardware_component_activity_fact_;
  const bool controller_activity_changed =
    observation.trajectory_controller_active !=
    last_controller_activity_fact_;
  const bool controller_inactive =
    observation.trajectory_controller_active == FactState::kFalse;
  const bool controller_active =
    observation.trajectory_controller_active == FactState::kTrue;
  if (controller_inactive) {
    trajectory_controller_active_ = false;
    controller_rearm_required_ = true;
    controller_seen_inactive_since_fence_ = true;
  } else if (controller_active) {
    trajectory_controller_active_ = true;
    if (controller_seen_inactive_since_fence_) {
      controller_rearm_required_ = false;
      controller_seen_inactive_since_fence_ = false;
    }
  } else {
    controller_rearm_required_ = true;
  }
  if (observation.generation == 0 ||
    observation.source_timestamp_ns <= 0 ||
    observation.source_timestamp_ns > now_ns ||
    observation.fresh_until_ns <= now_ns ||
    (backend_observation_generation_ != 0 &&
    observation.generation < backend_observation_generation_))
  {
    close_admission();
    return false;
  }
  last_hardware_component_activity_fact_ =
    observation.hardware_component_active;
  last_controller_activity_fact_ =
    observation.trajectory_controller_active;

  const std::int64_t source_timestamp_ns =
    observation.source_timestamp_ns;
  const std::int64_t fresh_until_ns = std::min(
    observation.fresh_until_ns,
    saturating_add(now_ns, observation_lease_ns_));
  const bool same_backend_observation =
    backend_observation_generation_ != 0 &&
    observation.generation == backend_observation_generation_;
  bool observation_renewed = false;
  if (same_backend_observation &&
    !hardware_activity_changed &&
    !controller_activity_changed)
  {
    observation_renewed = supervisor_.renew_observation_lease(
      observation_generation_, source_timestamp_ns, now_ns,
      fresh_until_ns);
  }
  if (!observation_renewed) {
    auto mapped = map_observation(observation, now_ns);
    if (!supervisor_.publish_observation(mapped)) {
      close_admission();
      return false;
    }
    ++capability_generation_;
    if (!supervisor_.replace_capabilities(
        capability_generation_, mapped.generation,
        map_capabilities(observation)))
    {
      close_admission();
      return false;
    }
    backend_observation_generation_ = observation.generation;
  }
  evaluate_pending(observation, now_ns);
  if (admission_was_open &&
    !supervisor_.motion_admitted(now_ns) &&
    is_true(observation.trajectory_controller_active))
  {
    require_controller_rearm();
  }
  publish_admission_cache(now_ns);
  return true;
}

bool XArmLifecycleCoordinator::establish_authority_session(
  const robot_lifecycle::AuthoritySession & session,
  std::int64_t now_ns)
{
  const auto previous = supervisor_.snapshot(now_ns);
  const bool established =
    supervisor_.establish_authority_session(session, now_ns);
  if (established &&
    (!previous.authority_session.has_value() ||
    previous.authority_session->session_id != session.session_id ||
    previous.authority_session->generation != session.generation ||
    previous.authority_session->principal_id != session.principal_id))
  {
    require_controller_rearm();
  }
  publish_admission_cache(now_ns);
  return established;
}

bool XArmLifecycleCoordinator::clear_authority_session(
  std::string_view session_id,
  std::uint64_t session_generation,
  std::int64_t now_ns)
{
  close_admission();
  const bool cleared = supervisor_.clear_authority_session(
    session_id, session_generation, now_ns);
  if (cleared) {
    require_controller_rearm();
  }
  publish_admission_cache(now_ns);
  return cleared;
}

robot_lifecycle::CommandDecision XArmLifecycleCoordinator::submit(
  const robot_lifecycle::CommandRequest & request,
  std::int64_t now_ns)
{
  const XArmActionPlan * plan = find_plan(request.action_id);
  const XArmTargetPlan * target = find_target_plan(request.action_id);
  bool preserve_admitted_target = false;
  if (target != nullptr &&
    target->action_id == robot_lifecycle::kPrepareForMotionActionId &&
    motion_admitted(now_ns) &&
    backend_command_gate_ready_ &&
    !controller_rearm_required_)
  {
    const XArmObservedState observation = backend_->observe(now_ns);
    preserve_admitted_target =
      observation.generation == backend_observation_generation_ &&
      postcondition_satisfied(*target, observation);
  }
  if (!preserve_admitted_target) {
    close_admission();
  }
  auto decision = supervisor_.submit(request, now_ns);
  if (decision.disposition !=
    robot_lifecycle::CommandDisposition::kAccepted)
  {
    publish_admission_cache(now_ns);
    return decision;
  }

  if (plan == nullptr && target == nullptr) {
    supervisor_.finish_failure(
      decision.transaction_generation, request.request_id,
      "xarm_action_plan_missing", now_ns);
    publish_admission_cache(now_ns);
    return decision;
  }
  if (!supervisor_.mark_executing(
      decision.transaction_generation, request.request_id, now_ns))
  {
    return decision;
  }

  pending_transaction_generation_ = decision.transaction_generation;
  pending_request_id_ = request.request_id;
  pending_action_id_ = request.action_id;
  pending_stage_action_id_.clear();
  pending_stage_sequence_ = 0;

  if (target != nullptr) {
    const XArmObservedState observation = backend_->observe(now_ns);
    if (observation.generation != backend_observation_generation_) {
      close_admission();
      supervisor_.finish_failure(
        decision.transaction_generation, request.request_id,
        "xarm_observation_changed_before_target_execution", now_ns);
      clear_pending_context();
      publish_admission_cache(now_ns);
      return decision;
    }
    if (postcondition_satisfied(*target, observation)) {
      if (supervisor_.mark_verifying(
          decision.transaction_generation, request.request_id, now_ns) &&
        supervisor_.finish_success(
          decision.transaction_generation, request.request_id,
          "xarm_target_already_satisfied", now_ns))
      {
        clear_pending_context();
      }
      publish_admission_cache(now_ns);
      return decision;
    }
    require_controller_rearm();
    advance_target(*target, observation, now_ns);
    publish_admission_cache(now_ns);
    return decision;
  }

  require_controller_rearm();
  PrimitiveContext context;
  context.transaction_generation = decision.transaction_generation;
  context.request_id = request.request_id;
  context.action_id = request.action_id;
  context.stage_action_id = request.action_id;
  context.step_index = 0;
  context.transport_request_id =
    "mf:" + request.request_id + ":step:0";
  context.expected_observation_generation =
    backend_observation_generation_;
  context.command = plan->steps.front();
  dispatch_primitive(std::move(context), now_ns);
  publish_admission_cache(now_ns);
  return decision;
}

bool XArmLifecycleCoordinator::reconcile(
  std::uint64_t transaction_generation,
  std::string_view request_id,
  std::string reason,
  std::int64_t now_ns)
{
  close_admission();
  if (deferred_primitive_.has_value()) {
    publish_admission_cache(now_ns);
    return false;
  }
  if (!pending_transaction_generation_.has_value() ||
    *pending_transaction_generation_ != transaction_generation ||
    pending_request_id_ != request_id)
  {
    publish_admission_cache(now_ns);
    return false;
  }
  if (!refresh(now_ns)) {
    return false;
  }
  const XArmActionPlan * plan = find_plan(pending_action_id_);
  const XArmTargetPlan * target = find_target_plan(pending_action_id_);
  if (plan == nullptr && target == nullptr) {
    publish_admission_cache(now_ns);
    return false;
  }
  const auto observation = backend_->observe(now_ns);
  const bool satisfied = target != nullptr ?
    postcondition_satisfied(*target, observation) :
    postcondition_satisfied(*plan, observation);
  const bool resolved = supervisor_.resolve_uncertain(
    transaction_generation, request_id, satisfied, std::move(reason),
    now_ns);
  if (resolved) {
    clear_pending_context();
  }
  publish_admission_cache(now_ns);
  return resolved;
}

bool XArmLifecycleCoordinator::complete_deferred_primitive(
  XArmLifecycleCommand command,
  std::string_view transport_request_id,
  std::uint64_t expected_observation_generation,
  const XArmLifecycleCommandResult & result,
  std::int64_t now_ns)
{
  if (!deferred_primitive_.has_value() ||
    deferred_primitive_->command != command ||
    deferred_primitive_->transport_request_id != transport_request_id ||
    deferred_primitive_->expected_observation_generation !=
    expected_observation_generation)
  {
    return false;
  }
  PrimitiveContext context = std::move(*deferred_primitive_);
  deferred_primitive_.reset();
  return apply_primitive_result(context, result, now_ns);
}

void XArmLifecycleCoordinator::set_hardware_component_active(bool active)
{
  close_admission();
  if (!active) {
    require_controller_rearm();
  }
  backend_->set_hardware_component_active(active);
}

void XArmLifecycleCoordinator::set_trajectory_controller_active(bool active)
{
  close_admission();
  trajectory_controller_active_ = active;
  if (!active) {
    controller_rearm_required_ = true;
    controller_seen_inactive_since_fence_ = true;
  } else if (controller_seen_inactive_since_fence_) {
    controller_rearm_required_ = false;
    controller_seen_inactive_since_fence_ = false;
  }
  backend_->set_trajectory_controller_active(active);
}

bool XArmLifecycleCoordinator::read_joint_state(JointStateSample & sample)
{
  return backend_->read_joint_state(sample);
}

bool XArmLifecycleCoordinator::write_joint_command(
  const JointCommandView & command,
  std::int64_t now_ns)
{
  if (!motion_admitted(now_ns)) {
    return false;
  }
  return backend_->write_joint_command(command);
}

bool XArmLifecycleCoordinator::motion_admitted(
  std::int64_t now_ns) const noexcept
{
  if (!motion_admitted_.load(std::memory_order_acquire)) {
    return false;
  }
  return now_ns <
         motion_admission_valid_until_ns_.load(std::memory_order_acquire);
}

robot_lifecycle::LifecycleSnapshot XArmLifecycleCoordinator::snapshot(
  std::int64_t now_ns) const
{
  auto current = supervisor_.snapshot(now_ns);
  const auto owner_progress = backend_->observe(now_ns);
  current.transport_owner_progress_sequence =
    owner_progress.transport_owner_progress_sequence;
  current.transport_owner_progress_received_timestamp_ns =
    owner_progress.transport_owner_received_timestamp_ns;
  current.transport_owner_progress_healthy =
    owner_progress.transport_owner_progress_healthy &&
    owner_progress.transport_owner_component_epoch ==
    current.observed.transport_owner_component_epoch;
  current.transport_owner_progress_reason =
    current.transport_owner_progress_healthy ?
    "" :
    owner_progress.transport_owner_component_epoch !=
    current.observed.transport_owner_component_epoch ?
    "transport_owner_component_epoch_mismatch" :
    owner_progress.transport_owner_progress_reason;
  if (controller_rearm_required_) {
    current.motion_admitted = false;
    if (std::find(
        current.admission_reason_codes.begin(),
        current.admission_reason_codes.end(),
        "trajectory_controller_rearm_required") ==
      current.admission_reason_codes.end())
    {
      current.admission_reason_codes.push_back(
        "trajectory_controller_rearm_required");
    }
  }
  if (!backend_command_gate_ready_) {
    current.motion_admitted = false;
    if (std::find(
        current.admission_reason_codes.begin(),
        current.admission_reason_codes.end(),
        "xarm_command_gate_closed") ==
      current.admission_reason_codes.end())
    {
      current.admission_reason_codes.push_back(
        "xarm_command_gate_closed");
    }
  }
  return current;
}

std::vector<XArmPrimitiveAuditRecord>
XArmLifecycleCoordinator::primitive_audit_snapshot() const
{
  return {primitive_audit_.begin(), primitive_audit_.end()};
}

void XArmLifecycleCoordinator::require_controller_rearm()
{
  controller_rearm_required_ = true;
  controller_seen_inactive_since_fence_ =
    !trajectory_controller_active_;
}

const XArmActionPlan * XArmLifecycleCoordinator::find_plan(
  std::string_view action_id) const
{
  const auto found = std::find_if(
    action_plans_.begin(), action_plans_.end(),
    [action_id](const XArmActionPlan & plan) {
      return plan.action_id == action_id;
    });
  return found == action_plans_.end() ? nullptr : &*found;
}

const XArmTargetPlan * XArmLifecycleCoordinator::find_target_plan(
  std::string_view action_id) const
{
  const auto found = std::find_if(
    target_plans_.begin(), target_plans_.end(),
    [action_id](const XArmTargetPlan & plan) {
      return plan.action_id == action_id;
    });
  return found == target_plans_.end() ? nullptr : &*found;
}

const XArmTargetStage *
XArmLifecycleCoordinator::find_eligible_target_stage(
  const XArmTargetPlan & target,
  const XArmObservedState & observation) const
{
  const auto found = std::find_if(
    target.stages.begin(), target.stages.end(),
    [&observation](const XArmTargetStage & stage) {
      return std::all_of(
        stage.eligibility.begin(), stage.eligibility.end(),
        [&observation](const XArmStateCondition & condition) {
          return condition_satisfied(condition, observation);
        });
    });
  return found == target.stages.end() ? nullptr : &*found;
}

robot_lifecycle::ObservedFacets XArmLifecycleCoordinator::map_observation(
  const XArmObservedState & observation,
  std::int64_t now_ns)
{
  robot_lifecycle::ObservedFacets mapped;
  mapped.generation = ++observation_generation_;
  mapped.transport_owner_component_epoch =
    observation.transport_owner_component_epoch;
  mapped.source_timestamp_ns = observation.source_timestamp_ns;
  mapped.received_timestamp_ns = now_ns;
  mapped.fresh_until_ns = std::min(
    observation.fresh_until_ns,
    saturating_add(now_ns, observation_lease_ns_));
  mapped.transport_connected = observation.transport_connected;
  mapped.identity_verified = observation.identity_verified;
  mapped.position_valid = observation.position_valid;
  mapped.robot_enabled = observation.motion_enabled;
  mapped.vendor_control_ready = observation.vendor_control_ready;
  mapped.hardware_component_active =
    observation.hardware_component_active;
  mapped.trajectory_controller_active =
    observation.trajectory_controller_active;
  mapped.fault_present = observation.fault_present;
  mapped.warning_present =
    observation.warning_code < 0 ? FactState::kUnknown :
    (observation.warning_code == 0 ? FactState::kFalse :
    FactState::kTrue);
  mapped.recovery_required = observation.recovery_required;
  mapped.stationary = observation.stationary;
  mapped.adapter_blocking_reason_codes =
    observation.blocking_reason_codes;
  mapped.adapter_status_keys = {
    "identity_axis",
    "identity_device_type",
    "vendor_mode",
    "vendor_state",
    "brake_mask",
    "servo_enable_mask",
    "error_code",
    "warning_code",
    "report_sample_count",
    "joint_read_success_count",
    "joint_write_attempt_count",
    "lifecycle_command_attempt_count",
  };
  mapped.adapter_status_values = {
    std::to_string(observation.identity_axis),
    std::to_string(observation.identity_device_type),
    std::to_string(observation.mode),
    std::to_string(observation.state),
    std::to_string(observation.brake_mask),
    std::to_string(observation.servo_enable_mask),
    std::to_string(observation.error_code),
    std::to_string(observation.warning_code),
    std::to_string(observation.report_sample_count),
    std::to_string(observation.joint_read_success_count),
    std::to_string(observation.joint_write_attempt_count),
    std::to_string(observation.lifecycle_command_attempt_count),
  };
  if (controller_rearm_required_) {
    mapped.adapter_blocking_reason_codes.push_back(
      "trajectory_controller_rearm_required");
  }
  return mapped;
}

std::vector<robot_lifecycle::CapabilityDescriptor>
XArmLifecycleCoordinator::map_capabilities(
  const XArmObservedState & observation) const
{
  std::vector<robot_lifecycle::CapabilityDescriptor> capabilities;
  capabilities.reserve(action_plans_.size() + target_plans_.size());
  for (const auto & plan : action_plans_) {
    robot_lifecycle::CapabilityDescriptor capability;
    capability.action_id = plan.action_id;
    capability.label = plan.label;
    capability.supported = true;
    capability.available = plan_available(plan, observation);
    capability.availability_reason =
      capability.available ? "" : "xarm_action_precondition_not_satisfied";
    capability.confirmation = plan.confirmation;
    capability.side_effect_summary = plan.side_effect_summary;
    capability.expected_postcondition = plan.expected_postcondition;
    capability.timeout_ns = plan.timeout_ns;
    capabilities.push_back(std::move(capability));
  }
  for (const auto & plan : target_plans_) {
    robot_lifecycle::CapabilityDescriptor capability;
    capability.action_id = plan.action_id;
    capability.label = plan.label;
    capability.supported = true;
    capability.available = target_plan_available(plan, observation);
    capability.availability_reason =
      capability.available ? "" :
      "xarm_target_precondition_not_satisfied";
    capability.confirmation = plan.confirmation;
    capability.side_effect_summary = plan.side_effect_summary;
    capability.expected_postcondition = plan.expected_postcondition;
    capability.timeout_ns = plan.timeout_ns;
    capabilities.push_back(std::move(capability));
  }
  return capabilities;
}

bool XArmLifecycleCoordinator::plan_available(
  const XArmActionPlan & plan,
  const XArmObservedState & observation) const
{
  return std::all_of(
    plan.preconditions.begin(), plan.preconditions.end(),
    [&observation](const XArmStateCondition & condition) {
      return condition_satisfied(condition, observation);
    });
}

bool XArmLifecycleCoordinator::target_plan_available(
  const XArmTargetPlan & plan,
  const XArmObservedState & observation) const
{
  if (postcondition_satisfied(plan, observation)) {
    return true;
  }
  if (!std::all_of(
      plan.preconditions.begin(), plan.preconditions.end(),
      [&observation](const XArmStateCondition & condition) {
        return condition_satisfied(condition, observation);
      }))
  {
    return false;
  }
  const XArmTargetStage * stage =
    find_eligible_target_stage(plan, observation);
  const XArmActionPlan * primitive =
    stage == nullptr ? nullptr : find_plan(stage->action_id);
  return primitive != nullptr &&
         plan_available(*primitive, observation);
}

bool XArmLifecycleCoordinator::postcondition_satisfied(
  const XArmActionPlan & plan,
  const XArmObservedState & observation) const
{
  return std::all_of(
    plan.postconditions.begin(), plan.postconditions.end(),
    [&observation](const XArmStateCondition & condition) {
      return condition_satisfied(condition, observation);
    });
}

bool XArmLifecycleCoordinator::postcondition_satisfied(
  const XArmTargetPlan & plan,
  const XArmObservedState & observation) const
{
  if (plan.action_id == robot_lifecycle::kPrepareForMotionActionId &&
    controller_rearm_required_)
  {
    return false;
  }
  return std::all_of(
    plan.postconditions.begin(), plan.postconditions.end(),
    [&observation](const XArmStateCondition & condition) {
      return condition_satisfied(condition, observation);
    });
}

bool XArmLifecycleCoordinator::advance_target(
  const XArmTargetPlan & target,
  const XArmObservedState & observation,
  std::int64_t now_ns)
{
  if (!pending_transaction_generation_.has_value() ||
    deferred_primitive_.has_value())
  {
    return false;
  }
  const auto current = supervisor_.snapshot(now_ns);
  if (!current.pending_transaction.has_value() ||
    current.pending_transaction->phase !=
    robot_lifecycle::TransactionPhase::kExecuting)
  {
    return false;
  }
  if (postcondition_satisfied(target, observation)) {
    if (!supervisor_.mark_verifying(
        *pending_transaction_generation_, pending_request_id_, now_ns))
    {
      return false;
    }
    const bool finished = supervisor_.finish_success(
      *pending_transaction_generation_, pending_request_id_,
      "xarm_target_postcondition_observed", now_ns);
    if (finished) {
      clear_pending_context();
    }
    return finished;
  }
  if (pending_stage_sequence_ == 0 &&
    !std::all_of(
      target.preconditions.begin(), target.preconditions.end(),
      [&observation](const XArmStateCondition & condition) {
        return condition_satisfied(condition, observation);
      }))
  {
    supervisor_.finish_failure(
      *pending_transaction_generation_, pending_request_id_,
      "xarm_target_precondition_changed", now_ns);
    clear_pending_context();
    return false;
  }

  const XArmTargetStage * stage =
    find_eligible_target_stage(target, observation);
  const XArmActionPlan * plan =
    stage == nullptr ? nullptr : find_plan(stage->action_id);
  if (stage == nullptr || plan == nullptr ||
    !plan_available(*plan, observation))
  {
    supervisor_.finish_failure(
      *pending_transaction_generation_, pending_request_id_,
      "xarm_target_no_eligible_stage", now_ns);
    clear_pending_context();
    return false;
  }

  const std::size_t stage_sequence = pending_stage_sequence_;
  PrimitiveContext context;
  context.transaction_generation = *pending_transaction_generation_;
  context.request_id = pending_request_id_;
  context.action_id = target.action_id;
  context.stage_action_id = stage->action_id;
  context.step_index = stage_sequence;
  context.transport_request_id =
    "mf:" + pending_request_id_ +
    ":stage:" + std::to_string(stage_sequence);
  context.expected_observation_generation =
    backend_observation_generation_;
  context.command = plan->steps.front();
  context.target_stage = true;
  return dispatch_primitive(std::move(context), now_ns);
}

bool XArmLifecycleCoordinator::dispatch_primitive(
  PrimitiveContext context,
  std::int64_t now_ns)
{
  const auto result = backend_->execute_lifecycle(
    context.command, context.transport_request_id,
    context.expected_observation_generation);
  if (result.state == XArmLifecycleResultState::kExecutionDeferred) {
    if (deferred_primitive_.has_value()) {
      supervisor_.mark_outcome_unknown(
        context.transaction_generation, context.request_id,
        "xarm_lifecycle_worker_overlap", now_ns);
      publish_admission_cache(now_ns);
      return false;
    }
    deferred_primitive_ = std::move(context);
    return true;
  }
  return apply_primitive_result(context, result, now_ns);
}

bool XArmLifecycleCoordinator::apply_primitive_result(
  const PrimitiveContext & context,
  const XArmLifecycleCommandResult & result,
  std::int64_t now_ns)
{
  record_primitive(
    now_ns, context.transaction_generation, context.action_id,
    context.stage_action_id, context.step_index,
    context.transport_request_id,
    context.expected_observation_generation, context.command, result);
  if (!pending_transaction_generation_.has_value() ||
    *pending_transaction_generation_ != context.transaction_generation ||
    pending_request_id_ != context.request_id ||
    pending_action_id_ != context.action_id)
  {
    return false;
  }
  const auto current = supervisor_.snapshot(now_ns);
  if (!current.pending_transaction.has_value() ||
    current.pending_transaction->generation !=
    context.transaction_generation ||
    current.pending_transaction->request_id != context.request_id ||
    current.pending_transaction->phase !=
    robot_lifecycle::TransactionPhase::kExecuting)
  {
    return false;
  }
  if (result.state ==
    XArmLifecycleResultState::kOutcomeUnknownAfterDispatch)
  {
    supervisor_.mark_outcome_unknown(
      context.transaction_generation, context.request_id,
      result.reason.empty() ?
      (context.target_stage ?
      "xarm_target_stage_transport_outcome_unknown" :
      "xarm_primitive_transport_outcome_unknown") : result.reason,
      now_ns);
    publish_admission_cache(now_ns);
    return false;
  }
  if (result.state != XArmLifecycleResultState::kResponseReceived ||
    !result.permitted || !result.attempted)
  {
    if (!context.target_stage && context.prior_step_attempted) {
      supervisor_.mark_outcome_unknown(
        context.transaction_generation, context.request_id,
        "xarm_partial_sequence_result_ambiguous", now_ns);
    } else {
      supervisor_.finish_failure(
        context.transaction_generation, context.request_id,
        result.reason.empty() ?
        (context.target_stage ?
        "xarm_target_stage_rejected" : "xarm_primitive_rejected") :
        result.reason,
        now_ns);
      clear_pending_context();
    }
    publish_admission_cache(now_ns);
    return false;
  }
  if (result.return_code != 0) {
    supervisor_.mark_outcome_unknown(
      context.transaction_generation, context.request_id,
      context.target_stage ?
      "xarm_target_stage_result_ambiguous" :
      "xarm_primitive_result_ambiguous",
      now_ns);
    publish_admission_cache(now_ns);
    return false;
  }

  if (context.target_stage) {
    pending_stage_action_id_ = context.stage_action_id;
    pending_stage_observation_generation_ = observation_generation_;
    pending_stage_sequence_ = context.step_index + 1;
    publish_admission_cache(now_ns);
    return true;
  }

  const XArmActionPlan * plan = find_plan(context.action_id);
  if (plan == nullptr ||
    context.step_index >= plan->steps.size() ||
    plan->steps[context.step_index] != context.command)
  {
    supervisor_.mark_outcome_unknown(
      context.transaction_generation, context.request_id,
      "xarm_action_plan_changed_during_execution", now_ns);
    publish_admission_cache(now_ns);
    return false;
  }
  const std::size_t next_step_index = context.step_index + 1;
  if (next_step_index < plan->steps.size()) {
    PrimitiveContext next = context;
    next.step_index = next_step_index;
    next.transport_request_id =
      "mf:" + context.request_id +
      ":step:" + std::to_string(next_step_index);
    next.expected_observation_generation =
      backend_observation_generation_;
    next.command = plan->steps[next_step_index];
    next.prior_step_attempted = true;
    return dispatch_primitive(std::move(next), now_ns);
  }

  if (plan->terminal_on_known_success) {
    if (!result.process_restart_required) {
      supervisor_.mark_outcome_unknown(
        context.transaction_generation, context.request_id,
        "xarm_controller_shutdown_restart_fence_missing", now_ns);
      publish_admission_cache(now_ns);
      return false;
    }
    if (!supervisor_.mark_verifying(
        context.transaction_generation, context.request_id, now_ns))
    {
      publish_admission_cache(now_ns);
      return false;
    }
    const bool finished = supervisor_.finish_success(
      context.transaction_generation, context.request_id,
      "controller_shutdown_vendor_accepted_process_restart_required",
      now_ns);
    if (finished) {
      clear_pending_context();
    }
    publish_admission_cache(now_ns);
    return finished;
  }

  if (!supervisor_.mark_verifying(
      context.transaction_generation, context.request_id, now_ns))
  {
    publish_admission_cache(now_ns);
    return false;
  }
  refresh(now_ns);
  return true;
}

void XArmLifecycleCoordinator::clear_pending_context()
{
  pending_transaction_generation_.reset();
  pending_request_id_.clear();
  pending_action_id_.clear();
  pending_stage_action_id_.clear();
  pending_stage_observation_generation_ = 0;
  pending_stage_sequence_ = 0;
}

void XArmLifecycleCoordinator::close_admission()
{
  if (backend_command_gate_ready_ && trajectory_controller_active_) {
    require_controller_rearm();
  }
  motion_admitted_.store(false, std::memory_order_release);
  motion_admission_valid_until_ns_.store(0, std::memory_order_release);
  backend_command_gate_ready_ = false;
  if (!backend_command_gate_known_closed_) {
    const auto update = backend_->set_command_gate(false, 0);
    backend_command_gate_known_closed_ =
      update.state == XArmCommandGateUpdateState::kApplied;
  }
}

void XArmLifecycleCoordinator::publish_admission_cache(
  std::int64_t now_ns)
{
  const auto current = supervisor_.snapshot(now_ns);
  if (controller_rearm_required_ ||
    !current.motion_admitted ||
    !current.authority_session.has_value())
  {
    close_admission();
    return;
  }
  const std::int64_t valid_until_ns = std::min(
    current.observed.fresh_until_ns,
    current.authority_session->valid_until_ns);
  const auto gate_update =
    backend_->set_command_gate(true, valid_until_ns);
  if (gate_update.state == XArmCommandGateUpdateState::kPending) {
    if (backend_command_gate_ready_ && trajectory_controller_active_) {
      require_controller_rearm();
    }
    motion_admitted_.store(false, std::memory_order_release);
    motion_admission_valid_until_ns_.store(0, std::memory_order_release);
    backend_command_gate_ready_ = false;
    backend_command_gate_known_closed_ = false;
    return;
  }
  if (gate_update.state != XArmCommandGateUpdateState::kApplied ||
    gate_update.effective_valid_until_ns <= now_ns)
  {
    close_admission();
    return;
  }
  backend_command_gate_ready_ = true;
  backend_command_gate_known_closed_ = false;
  motion_admission_valid_until_ns_.store(
    std::min(
      valid_until_ns, gate_update.effective_valid_until_ns),
    std::memory_order_release);
  motion_admitted_.store(true, std::memory_order_release);
}

bool XArmLifecycleCoordinator::evaluate_pending(
  const XArmObservedState & observation,
  std::int64_t now_ns)
{
  if (!pending_transaction_generation_.has_value()) {
    return false;
  }
  if (deferred_primitive_.has_value()) {
    return false;
  }
  const auto current = supervisor_.snapshot(now_ns);
  if (!current.pending_transaction.has_value() ||
    current.pending_transaction->phase ==
    robot_lifecycle::TransactionPhase::kOutcomeUnknown)
  {
    return false;
  }

  if (const XArmTargetPlan * target =
    find_target_plan(pending_action_id_))
  {
    if (current.pending_transaction->phase !=
      robot_lifecycle::TransactionPhase::kExecuting)
    {
      return false;
    }
    if (!pending_stage_action_id_.empty()) {
      if (observation_generation_ <=
        pending_stage_observation_generation_)
      {
        return false;
      }
      const XArmActionPlan * stage =
        find_plan(pending_stage_action_id_);
      if (stage == nullptr ||
        !postcondition_satisfied(*stage, observation))
      {
        return false;
      }
      pending_stage_action_id_.clear();
    }
    return advance_target(*target, observation, now_ns);
  }

  if (current.pending_transaction->phase !=
    robot_lifecycle::TransactionPhase::kVerifying)
  {
    return false;
  }
  const XArmActionPlan * plan = find_plan(pending_action_id_);
  if (plan == nullptr || !postcondition_satisfied(*plan, observation)) {
    return false;
  }
  const bool finished = supervisor_.finish_success(
    *pending_transaction_generation_, pending_request_id_,
    "xarm_postcondition_observed", now_ns);
  if (finished) {
    clear_pending_context();
  }
  return finished;
}

void XArmLifecycleCoordinator::record_primitive(
  std::int64_t now_ns,
  std::uint64_t transaction_generation,
  std::string_view action_id,
  std::string_view stage_action_id,
  std::size_t step_index,
  std::string transport_request_id,
  std::uint64_t expected_observation_generation,
  XArmLifecycleCommand command,
  const XArmLifecycleCommandResult & result)
{
  XArmPrimitiveAuditRecord record;
  record.sequence = next_primitive_audit_sequence_++;
  record.occurred_at_ns = now_ns;
  record.transaction_generation = transaction_generation;
  record.action_id = action_id;
  record.stage_action_id = stage_action_id;
  record.step_index = step_index;
  record.transport_request_id = std::move(transport_request_id);
  record.expected_observation_generation =
    expected_observation_generation;
  record.command = command;
  record.result = result;
  primitive_audit_.push_back(std::move(record));
  while (primitive_audit_.size() > primitive_audit_capacity_) {
    primitive_audit_.pop_front();
  }
}

}  // namespace manyforge::xarm_control
