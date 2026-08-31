#include <algorithm>
#include <cmath>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/runtime_participant_policy.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
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

using MotionScopeKey = std::tuple<std::string_view, std::string_view, std::string_view>;

[[nodiscard]] MotionScopeKey motion_scope_key(const RuntimePolicyMotionScopeDTO& scope) noexcept {
  return std::visit(
      [](const auto& value) -> MotionScopeKey {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, RuntimePolicyCellScopeDTO>) {
          return {"cell", {}, {}};
        } else {
          return {"command_target", value.controllable_component_id.value(),
                  value.command_target_id.value()};
        }
      },
      scope);
}

[[nodiscard]] std::tuple<std::string_view, std::string_view, std::uint8_t> acknowledgement_key(
    const RuntimeOpenLoopGraspAcknowledgementDTO& value) noexcept {
  return {value.controllable_component_id.value(), value.command_target_id.value(),
          static_cast<std::uint8_t>(value.environment)};
}

[[nodiscard]] bool matches(const RuntimePolicyMotionScopeDTO& scope,
                           const RuntimeParticipantBindingDTO& binding) noexcept {
  return std::visit(
      [&](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, RuntimePolicyCellScopeDTO>) {
          return true;
        } else {
          return value.controllable_component_id == binding.controllable_component_id &&
                 value.command_target_id == binding.command_target_id;
        }
      },
      scope);
}

}  // namespace

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
  if (policy.motion_speed_limits.empty() ||
      policy.motion_speed_limits.size() > kMaxRuntimePolicyRules ||
      !std::is_sorted(policy.motion_speed_limits.begin(), policy.motion_speed_limits.end(),
                      [](const auto& left, const auto& right) {
                        return motion_scope_key(left.scope) < motion_scope_key(right.scope);
                      }) ||
      std::adjacent_find(policy.motion_speed_limits.begin(), policy.motion_speed_limits.end(),
                         [](const auto& left, const auto& right) {
                           return motion_scope_key(left.scope) == motion_scope_key(right.scope);
                         }) != policy.motion_speed_limits.end()) {
    return invalid<RuntimeParticipantPolicySelection>(
        "runtime policy motion limits are empty, over-bound, duplicate, or noncanonical",
        "runtime_policy_motion_limits_invalid");
  }

  std::optional<double> cell_limit;
  std::optional<double> exact_target_limit;
  std::optional<RuntimeMotionSpeedLimitDTO> cell_limit_row;
  std::optional<RuntimeMotionSpeedLimitDTO> exact_target_limit_row;
  for (const auto& limit : policy.motion_speed_limits) {
    if (!std::isfinite(limit.maximum_tcp_linear_speed_mps) ||
        limit.maximum_tcp_linear_speed_mps <= 0.0 || limit.maximum_tcp_linear_speed_mps > 1000.0) {
      return invalid<RuntimeParticipantPolicySelection>(
          "runtime policy motion limit is outside the closed finite range",
          "runtime_policy_motion_limit_value_invalid");
    }
    if (std::holds_alternative<RuntimePolicyCellScopeDTO>(limit.scope)) {
      cell_limit = limit.maximum_tcp_linear_speed_mps;
      cell_limit_row = limit;
      continue;
    }
    const auto& target = std::get<RuntimePolicyCommandTargetScopeDTO>(limit.scope);
    if (!bounded_id(target.controllable_component_id) || !bounded_id(target.command_target_id)) {
      return invalid<RuntimeParticipantPolicySelection>(
          "runtime policy contains an invalid target-scoped motion limit",
          "runtime_policy_motion_limit_scope_invalid");
    }
    if (matches(limit.scope, participant.value())) {
      exact_target_limit = limit.maximum_tcp_linear_speed_mps;
      exact_target_limit_row = limit;
    }
  }
  if (cell_limit.has_value() && exact_target_limit.has_value() &&
      *exact_target_limit > *cell_limit) {
    return invalid<RuntimeParticipantPolicySelection>(
        "target-scoped runtime limit weakens the cell-wide ceiling",
        "runtime_policy_target_limit_weakens_cell");
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

  if (!cell_limit.has_value() && !exact_target_limit.has_value()) {
    return invalid<RuntimeParticipantPolicySelection>(
        "runtime policy has no speed limit matching the exact participant",
        "runtime_policy_participant_scope_unmatched");
  }
  const double effective =
      cell_limit.has_value()
          ? (exact_target_limit.has_value() ? std::min(*cell_limit, *exact_target_limit)
                                            : *cell_limit)
          : *exact_target_limit;
  const auto& selected_limit = exact_target_limit_row.has_value()
                                   ? *exact_target_limit_row
                                   : *cell_limit_row;
  return Result<RuntimeParticipantPolicySelection>::Ok(
      RuntimeParticipantPolicySelection(RuntimeParticipantPolicySelectionDTO{
          participant.value().controllable_component_id,
          participant.value().command_target_id, std::move(source_artifact),
          std::move(policy), selected_limit, effective}));
}

Result<SpeedProfileDTO> apply_runtime_participant_motion_speed_policy(
    const RuntimeParticipantPolicySelection& selection, SpeedProfileDTO requested) {
  if ((requested.max_tcp_linear_speed_mps.has_value() &&
       (!std::isfinite(*requested.max_tcp_linear_speed_mps) ||
        *requested.max_tcp_linear_speed_mps <= 0.0)) ||
      !std::isfinite(selection.value().effective_maximum_tcp_linear_speed_mps) ||
      selection.value().effective_maximum_tcp_linear_speed_mps <= 0.0) {
    return invalid<SpeedProfileDTO>("motion speed profile or sealed runtime ceiling is invalid",
                                    "runtime_policy_speed_profile_invalid");
  }
  requested.max_tcp_linear_speed_mps =
      std::min(requested.max_tcp_linear_speed_mps.value_or(
                   selection.value().effective_maximum_tcp_linear_speed_mps),
               selection.value().effective_maximum_tcp_linear_speed_mps);
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
