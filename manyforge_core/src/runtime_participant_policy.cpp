#include <algorithm>
#include <cmath>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/runtime_participant_policy.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

template <typename Value>
[[nodiscard]] Result<Value> invalid(std::string message, std::string reason) {
  return Result<Value>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                     std::move(message),
                                     {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool bounded_nfc(std::string_view value, std::size_t maximum,
                               bool permit_empty = false) noexcept {
  if ((!permit_empty && value.empty()) || value.size() > maximum ||
      value.find('\0') != std::string_view::npos) {
    return false;
  }
  try {
    return content_identity::is_utf8_nfc(value);
  } catch (...) {
    return false;
  }
}

template <typename Id>
[[nodiscard]] bool bounded_id(const Id& value) noexcept {
  return bounded_nfc(value.value(), 1024U);
}

[[nodiscard]] std::tuple<std::string_view, std::string_view, std::uint8_t> acknowledgement_key(
    const RuntimeOpenLoopGraspAcknowledgementDTO& value) noexcept {
  return {value.controllable_component_id.value(), value.command_target_id.value(),
          static_cast<std::uint8_t>(value.environment)};
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

Result<RuntimeParticipantPolicySelection> verify_runtime_participant_policy_selection(
    RuntimePolicyCatalogRecordRefDTO source_artifact, RuntimePolicyDTO policy,
    const RuntimeParticipantBinding& participant) {
  if (source_artifact.record_id != policy.runtime_policy_id ||
      source_artifact.record_revision != policy.runtime_policy_revision ||
      source_artifact.schema_id.value() != kRuntimePolicyApiVersion ||
      !IsExactLowercaseSha256Digest(source_artifact.schema_revision.value()) ||
      !IsExactLowercaseSha256Digest(source_artifact.content_digest.value()) ||
      source_artifact.content_addressed_store_key.empty() ||
      !bounded_id(policy.runtime_policy_id) ||
      !IsExactLowercaseSha256Digest(policy.runtime_policy_revision.value()) ||
      (policy.description.has_value() &&
       !bounded_nfc(*policy.description, kMaxRuntimePolicyDescriptionBytes, true)) ||
      policy.interrupt_policy_ids.size() > kMaxRuntimePolicyRules ||
      !std::is_sorted(policy.interrupt_policy_ids.begin(), policy.interrupt_policy_ids.end()) ||
      std::adjacent_find(policy.interrupt_policy_ids.begin(), policy.interrupt_policy_ids.end()) !=
          policy.interrupt_policy_ids.end() ||
      std::any_of(policy.interrupt_policy_ids.begin(), policy.interrupt_policy_ids.end(),
                  [](const auto& value) { return !bounded_id(value); })) {
    return invalid<RuntimeParticipantPolicySelection>(
        "runtime policy identity, description, or interrupt-policy set is invalid",
        "runtime_policy_shape_invalid");
  }
  if (policy.open_loop_grasp_acknowledgements.size() > kMaxRuntimePolicyRules ||
      !std::is_sorted(policy.open_loop_grasp_acknowledgements.begin(),
                      policy.open_loop_grasp_acknowledgements.end(),
                      [](const auto& left, const auto& right) {
                        return acknowledgement_key(left) < acknowledgement_key(right);
                      }) ||
      std::adjacent_find(policy.open_loop_grasp_acknowledgements.begin(),
                         policy.open_loop_grasp_acknowledgements.end(),
                         [](const auto& left, const auto& right) {
                           return acknowledgement_key(left) == acknowledgement_key(right);
                         }) != policy.open_loop_grasp_acknowledgements.end() ||
      std::any_of(policy.open_loop_grasp_acknowledgements.begin(),
                  policy.open_loop_grasp_acknowledgements.end(), [](const auto& value) {
                    return !bounded_id(value.controllable_component_id) ||
                           !bounded_id(value.command_target_id) ||
                           (value.environment != OpenLoopGraspEnvironmentDTO::kSimulation &&
                            value.environment !=
                                OpenLoopGraspEnvironmentDTO::kCommissionedHardware);
                  })) {
    return invalid<RuntimeParticipantPolicySelection>(
        "runtime policy open-loop acknowledgement set is invalid",
        "runtime_policy_open_loop_acknowledgements_invalid");
  }

  return Result<RuntimeParticipantPolicySelection>::Ok(
      RuntimeParticipantPolicySelection(RuntimeParticipantPolicySelectionDTO{
          participant.value().controllable_component_id, participant.value().command_target_id,
          std::move(source_artifact), std::move(policy)}));
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

Result<void> validate_runtime_participant_interrupt_policy_selection(
    const RuntimeParticipantPolicySelection& selection,
    std::span<const MotionInterruptScopeDTO> scopes) {
  const auto& allowed = selection.value().policy.interrupt_policy_ids;
  for (const auto& scope : scopes) {
    const InterruptPolicyId policy_id(scope.policy_id);
    if (scope.policy_id.empty() || !std::binary_search(allowed.begin(), allowed.end(), policy_id)) {
      return invalid<void>("motion interrupt scope is not selected by the sealed runtime policy",
                           "runtime_policy_interrupt_policy_not_selected");
    }
  }
  return Result<void>::Ok();
}

}  // namespace manyforge::core
