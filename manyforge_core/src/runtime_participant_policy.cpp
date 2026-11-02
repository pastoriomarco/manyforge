#include <algorithm>
#include <cmath>
#include <manyforge_core/runtime_participant_policy.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <optional>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

template <typename Value>
[[nodiscard]] Result<Value> invalid(std::string message, std::string reason) {
  return Result<Value>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                     std::move(message),
                                     {{"reason", std::move(reason)}}});
}

}  // namespace

Result<RuntimeParticipantPolicySelection> apply_deployment_motion_jerk_policy(
    const RuntimeParticipantPolicySelection& selection, const RuntimeParticipantBinding& binding,
    OrdinaryMotionJerkPolicy profile) {
  const auto& target = binding.value();
  if (selection.value().controllable_component_id != target.controllable_component_id ||
      selection.value().command_target_id != target.command_target_id ||
      profile.limits.size() > target.resources.size() ||
      (profile.enabled && profile.limits.size() != target.resources.size()))
    return invalid<RuntimeParticipantPolicySelection>(
        "ordinary jerk policy is incomplete or crosses its target",
        "ordinary_motion_jerk_policy_invalid");
  for (const auto& [joint, limit] : profile.limits) {
    const auto resource =
        std::find_if(target.resources.begin(), target.resources.end(),
                     [&](const auto& row) { return row.source_joint_name == joint; });
    const auto* angular = std::get_if<AngularJerkDTO>(&limit);
    // The existing ordinary per-joint dynamics port is angular. Do not
    // reinterpret a linear value as radians to admit an unsupported target.
    if (resource == target.resources.end() ||
        resource->coordinate_kind != CoordinateKind::kAngular || angular == nullptr ||
        !std::isfinite(angular->value_rad_s3) || angular->value_rad_s3 <= 0.0)
      return invalid<RuntimeParticipantPolicySelection>(
          "ordinary jerk limit has an invalid joint, coordinate kind or value",
          "ordinary_motion_jerk_limit_invalid");
  }
  auto value = selection.value();
  value.ordinary_motion_jerk = std::move(profile);
  return Result<RuntimeParticipantPolicySelection>::Ok(
      RuntimeParticipantPolicySelection(std::move(value)));
}

Result<PerJointDynamicsLimitsDTO> apply_runtime_participant_motion_jerk_policy(
    const RuntimeParticipantPolicySelection& selection, PerJointDynamicsLimitsDTO requested) {
  const auto& profile = selection.value().ordinary_motion_jerk;
  if (!profile) return Result<PerJointDynamicsLimitsDTO>::Ok(std::move(requested));
  if (!profile->enabled) {
    if (std::any_of(requested.begin(), requested.end(),
                    [](const auto& row) { return row.second.max_jerk_rad_s3.has_value(); }))
      return invalid<PerJointDynamicsLimitsDTO>(
          "ordinary-motion jerk shaping is disabled in the applied Deployment",
          "ordinary_motion_jerk_disabled");
    return Result<PerJointDynamicsLimitsDTO>::Ok(std::move(requested));
  }
  for (const auto& [joint, limit] : profile->limits) {
    const double maximum = std::get<AngularJerkDTO>(limit).value_rad_s3;
    auto& cap = requested[joint].max_jerk_rad_s3;
    if (cap && (!std::isfinite(*cap) || *cap <= 0.0 || *cap > maximum))
      return invalid<PerJointDynamicsLimitsDTO>(
          "requested jerk exceeds the applied Deployment limit for " + joint,
          "ordinary_motion_jerk_limit_exceeded");
    if (!cap) cap = maximum;
  }
  return Result<PerJointDynamicsLimitsDTO>::Ok(std::move(requested));
}

RuntimeParticipantPolicySelection select_runtime_participant_policy(
    const RuntimeParticipantBinding& participant) {
  return RuntimeParticipantPolicySelection(RuntimeParticipantPolicySelectionDTO{
      participant.value().controllable_component_id, participant.value().command_target_id});
}

Result<SpeedProfileDTO> apply_deployment_motion_speed_limit(
    const double deployment_maximum_tcp_linear_speed_mps, SpeedProfileDTO requested) {
  if ((requested.max_tcp_linear_speed_mps.has_value() &&
       (!std::isfinite(*requested.max_tcp_linear_speed_mps) ||
        *requested.max_tcp_linear_speed_mps <= 0.0)) ||
      !std::isfinite(deployment_maximum_tcp_linear_speed_mps) ||
      deployment_maximum_tcp_linear_speed_mps <= 0.0 ||
      deployment_maximum_tcp_linear_speed_mps > 1000.0) {
    return invalid<SpeedProfileDTO>(
        "motion speed request or applied Deployment maximum is invalid",
        "deployment_motion_speed_invalid");
  }
  requested.max_tcp_linear_speed_mps =
      std::min(requested.max_tcp_linear_speed_mps.value_or(
                   deployment_maximum_tcp_linear_speed_mps),
               deployment_maximum_tcp_linear_speed_mps);
  return Result<SpeedProfileDTO>::Ok(std::move(requested));
}

Result<void> validate_motion_interrupt_scope_admission(
    std::span<const MotionInterruptScopeDTO> scopes,
    std::span<const IoMotionRulePolicyBindingDTO> policies,
    const RuntimeParticipantBindingDTO* participant) {
  if (!scopes.empty() && policies.empty())
    return invalid<void>("motion interrupt scopes have no installed sealed policy binding",
                         "motion_interrupt_policy_not_admitted");
  std::set<std::string> ids;
  for (const auto& scope : scopes) {
    if (scope.scope_id.empty() || scope.policy_id.empty() || !scope.required ||
        scope.result_binding_key || !ids.insert(scope.scope_id).second ||
        (scope.arm_timeout_ns && *scope.arm_timeout_ns < 0))
      return invalid<void>("motion interrupt scope is invalid", "motion_interrupt_scope_invalid");
    const auto policy = std::find_if(policies.begin(), policies.end(), [&](const auto& candidate) {
      return candidate.policy_id.value() == scope.policy_id;
    });
    if (policy == policies.end() || !policy->condition || policy->effective_priority < 0 ||
        policy->effective_priority > 1000 ||
        std::count_if(policies.begin(), policies.end(), [&](const auto& candidate) {
          return candidate.policy_id.value() == scope.policy_id;
        }) != 1)
      return invalid<void>("motion interrupt policy binding is not installed exactly once",
                           "motion_interrupt_policy_not_admitted");
    if (!policy->semantics || policy->semantics->scope_kind != InterruptPolicyScopeKind::kMove ||
        !policy->semantics->affected_phases.contains(OperationStateKind::kExecuting))
      return invalid<void>("motion interrupt policy has no qualified operation semantics",
                           "motion_interrupt_action_not_admitted");
    const auto& semantics = *policy->semantics;
    if (!policy->maximum_scope || !participant || !participant->control_path)
      return invalid<void>("motion interrupt maximum scope is not qualified",
                           "motion_interrupt_scope_not_admitted");
    const auto& maximum = *policy->maximum_scope;
    const auto canonical = [](const auto& values) {
      return values.size() <= 4096U && std::is_sorted(values.begin(), values.end()) &&
             std::adjacent_find(values.begin(), values.end()) == values.end() &&
             std::all_of(values.begin(), values.end(),
                         [](const auto& value) { return !value.value().empty(); });
    };
    const auto admits = [](const auto& values, const auto& value) {
      return values.empty() || std::binary_search(values.begin(), values.end(), value);
    };
    const auto& control = *participant->control_path;
    if ((maximum.resource_ids.empty() && maximum.control_domain_ids.empty()) ||
        !canonical(maximum.controllable_component_ids) || !canonical(maximum.command_target_ids) ||
        !canonical(maximum.control_domain_ids) || !canonical(maximum.control_binding_ids) ||
        !canonical(maximum.resource_ids) ||
        (maximum.stop_scope != IoRuleStopScopeDTO::kAffectedResources &&
         maximum.stop_scope != IoRuleStopScopeDTO::kFullCell) ||
        !admits(maximum.controllable_component_ids, participant->controllable_component_id) ||
        !admits(maximum.command_target_ids, participant->command_target_id) ||
        !admits(maximum.control_domain_ids, control.control_domain_id) ||
        !admits(maximum.control_binding_ids, control.control_binding_id) ||
        !std::all_of(participant->resources.begin(), participant->resources.end(),
                     [&](const auto& resource) {
                       return admits(maximum.resource_ids, resource.resource_id);
                     }))
      return invalid<void>("motion participant exceeds the sealed interrupt maximum scope",
                           "motion_interrupt_scope_not_admitted");
    std::set<InterruptPolicyId> maskable_by;
    if (semantics.maskable_by.size() > 4096U ||
        !std::all_of(
            semantics.maskable_by.begin(), semantics.maskable_by.end(),
            [&](const auto& id) { return !id.value().empty() && maskable_by.insert(id).second; }))
      return invalid<void>("motion interrupt masking references are invalid",
                           "motion_interrupt_mask_not_admitted");
    if ((policy->stop_entry_stage_id && policy->stop_entry_stage_id->value().empty()) ||
        (policy->success_through_stage_id && policy->success_through_stage_id->value().empty()))
      return invalid<void>("interrupt Stop response selection is invalid",
                           "motion_interrupt_stop_response_not_admitted");
    const bool desired =
        semantics.preemption_action == InterruptPreemptionAction::kCompleteMoveOnSignal;
    const bool fault = semantics.preemption_action == InterruptPreemptionAction::kPreemptTrajectory;
    if ((desired && (!scope.stop_profile || !scope.stop_profile->require_backend_confirmation ||
                     semantics.affected_phases != std::set{OperationStateKind::kExecuting} ||
                     semantics.post_latch_disposition !=
                         InterruptPostLatchDisposition::kAutoClearAfterMotionOutcome)) ||
        (fault &&
         (semantics.post_latch_disposition != InterruptPostLatchDisposition::kFaultTerminal ||
          !std::all_of(semantics.affected_phases.begin(), semantics.affected_phases.end(),
                       [](const auto phase) {
                         return phase == OperationStateKind::kDispatching ||
                                phase == OperationStateKind::kExecuting ||
                                phase == OperationStateKind::kStopping ||
                                phase == OperationStateKind::kInterventionRequired;
                       }) ||
          !policy->allowed_stop_modes.empty() || scope.stop_profile.has_value() ||
          policy->success_through_stage_id.has_value())) ||
        (!desired && !fault))
      return invalid<void>("motion interrupt action/disposition is unsupported or inconsistent",
                           "motion_interrupt_action_not_admitted");
    const bool activation_allowed =
        policy->allowed_activation_modes.empty()
            ? scope.activation_mode == MotionInterruptActivationMode::kRequireInactiveAtArm
            : policy->allowed_activation_modes.contains(scope.activation_mode);
    if (!activation_allowed ||
        scope.activation_mode == MotionInterruptActivationMode::kMaintainLevelWithHysteresis ||
        (desired &&
         (!policy->allowed_stop_modes.contains(scope.stop_profile->stop_mode) ||
          std::any_of(scope.stop_profile->fallback_stop_modes.begin(),
                      scope.stop_profile->fallback_stop_modes.end(), [&](const auto mode) {
                        return !policy->allowed_stop_modes.contains(mode);
                      }))))
      return invalid<void>("motion interrupt mode is outside the sealed policy binding",
                           "motion_interrupt_mode_not_admitted");
  }
  return Result<void>::Ok();
}

}  // namespace manyforge::core
