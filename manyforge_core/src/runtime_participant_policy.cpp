#include <algorithm>
#include <cmath>
#include <manyforge_core/runtime_participant_policy.hpp>
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
      (profile.machine_required && !profile.enabled) ||
      profile.limits.size() > target.resources.size() ||
      (profile.enabled && profile.limits.size() != target.resources.size()))
    return invalid<RuntimeParticipantPolicySelection>(
        "ordinary jerk policy is incomplete, crosses its target or disables required limits",
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
  if (const auto& required = selection.value().ordinary_motion_jerk;
      required && required->machine_required) {
    if (!profile.enabled || !profile.machine_required ||
        std::any_of(required->limits.begin(), required->limits.end(), [&](const auto& row) {
          const auto found = profile.limits.find(row.first);
          return found == profile.limits.end() ||
                 std::get<AngularJerkDTO>(found->second).value_rad_s3 >
                     std::get<AngularJerkDTO>(row.second).value_rad_s3;
        }))
      return invalid<RuntimeParticipantPolicySelection>(
          "ordinary jerk policy cannot weaken an existing required limit",
          "ordinary_motion_jerk_policy_weakens_required");
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
    std::span<const MotionInterruptScopeDTO> scopes) {
  if (!scopes.empty()) {
    return invalid<void>(
        "motion interrupt scopes are not admitted: no per-robot motion-interrupt policy exists",
        "motion_interrupt_policy_not_admitted");
  }
  return Result<void>::Ok();
}

}  // namespace manyforge::core
