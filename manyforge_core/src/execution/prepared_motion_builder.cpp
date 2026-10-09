#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/execution/prepared_motion_builder.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_planning_codec.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Cold path. Field derivation (every `PreparedMotion` field, nothing from
// transport):
//   key                         the Kernel-minted key, bound to the decoded
//                               participant's operation and binding identities
//   prepared_contract_digest    the decoded participant's recomputed digest
//   trajectory                  the participant's sealed motion trajectory
//   interpolation               quintic: the sealed trajectory codec requires
//                               complete P/V/A knots with runtime scale bounds
//   joint_count                 trajectory width (1..kMaxMotionJoints)
//   runtime_scale_bounds        sealed trajectory bounds, unchanged
//   governor_bounds             sealed ordinary rate / rate-change bounds
//   effective_limits            the participant's motion request
//                               `per_resource_dynamics_limits` per resource
//   state_authority             binding control path's state authority class
//   final_position_tolerance    the participant's motion request
//                               `tolerances.joint_position_tol_rad`
//   rest_velocity_threshold     min(retained default settle threshold, every
//                               prepared controlled-Stop stage's threshold)
// `stop_braking_bounds` and `tick_envelope` are `StepMotion` in-loop braking
// inputs and are not derived here.

namespace manyforge::core {
namespace {

class Refused final : public std::runtime_error {
 public:
  Refused(std::string reason, std::map<std::string, std::string> details = {})
      : std::runtime_error(reason), reason_(std::move(reason)), details_(std::move(details)) {}
  [[nodiscard]] ErrorDTO error() const {
    auto details = details_;
    details.insert_or_assign("reason", reason_);
    return ErrorDTO{"prepared_motion_refused",
                    "prepared motion cannot be built from these sealed inputs", std::move(details)};
  }

 private:
  std::string reason_;
  std::map<std::string, std::string> details_;
};

[[noreturn]] void refuse(std::string reason, std::map<std::string, std::string> details = {}) {
  throw Refused(std::move(reason), std::move(details));
}

[[noreturn]] void refuse_nested(std::string reason, const ErrorDTO& nested) {
  auto details = nested.details;
  const auto inner = details.find("reason");
  details["nested_reason"] = inner == details.end() ? nested.code : inner->second;
  details.erase("reason");
  details["nested_code"] = nested.code;
  refuse(std::move(reason), std::move(details));
}

bool positive_finite(double value) noexcept {
  return std::isfinite(value) && value > 0.0;
}

double magnitude(const CoordinateVelocityDTO& value) {
  return std::visit(
      [](const auto& typed) {
        if constexpr (requires { typed.value_rad_s; }) {
          return typed.value_rad_s;
        } else {
          return typed.value_m_s;
        }
      },
      value);
}

struct LabelledPlan {
  std::string label;
  const PreparedParticipantStopPlanDTO* plan{};
};

// Strictest settle velocity threshold per trajectory joint: the retained
// default and every prepared controlled-Stop stage of every retained Stop
// plan the Kernel may select for this participant.
std::array<double, kMaxMotionJoints> rest_velocity_thresholds(
    std::span<const LabelledPlan> plans, std::span<const RuntimeResourceBindingDTO* const> joints,
    std::span<const RuntimeResourceBindingDTO> resources, std::span<const double> default_settle) {
  std::array<double, kMaxMotionJoints> settle{};
  std::copy(default_settle.begin(), default_settle.end(), settle.begin());
  for (const auto& [label, plan] : plans) {
    for (const auto& stage : plan->stage_commands) {
      if (stage.resolved_stage.containment_command.command_kind !=
              StopContainmentCommandKind::kMotionControlledStop ||
          !stage.validated_contract_parameters.has_value()) {
        continue;
      }
      // The one shared inverse projection of the prepared provider contract.
      const auto profile = map_motion_controlled_stop_contract_parameters_to_profile(
          *stage.validated_contract_parameters, resources, "prepared_stop_stage");
      if (!profile.ok()) {
        const auto& details = profile.error().details;
        refuse("prepared_motion_stop_stage_parameters_invalid",
               {{"stop_plan", label},
                {"stage_index", std::to_string(stage.resolved_stage.stage_index)},
                {"nested_reason", details.contains("reason") ? details.at("reason") : ""}});
      }
      if (!profile.value().per_joint_settle_velocity_threshold) continue;
      const auto& thresholds = *profile.value().per_joint_settle_velocity_threshold;
      for (std::size_t j = 0U; j < joints.size(); ++j) {
        const auto threshold = thresholds.find(*joints[j]->source_joint_name);
        if (threshold != thresholds.end()) {
          settle[j] = std::min(settle[j], magnitude(threshold->second));
        }
      }
    }
  }
  return settle;
}

PreparedMotionSlot build(const SealedPreparedMotionInputs& sealed, const ExecutionGoalKey& key,
                         const RuntimeParticipantBinding& binding,
                         const RuntimeParticipantStopReadiness& readiness,
                         std::span<const HardStopResourceLimitDTO> retained_default_limits,
                         MotionInterruptNodeCategory node_category) {
  // Sealed participant: decode re-verifies every referent and recomputes the
  // digest; the transported digest must be exactly that digest.
  auto decoded =
      DecodePreparedOperationParticipant(sealed.canonical_participant_bytes, binding, readiness,
                                         retained_default_limits, node_category);
  if (!decoded.ok()) refuse_nested("prepared_motion_participant_invalid", decoded.error());
  const auto& participant = decoded.value();
  if (!content_identity::digest_equal(participant.prepared_contract_digest.value(),
                                      sealed.participant_digest.value())) {
    refuse("prepared_motion_participant_digest_mismatch");
  }
  const auto* motion = std::get_if<PreparedMotionExecutionDTO>(&participant.payload);
  if (motion == nullptr) refuse("prepared_motion_payload_not_motion");
  auto trajectory = DecodePreparedTrajectory(sealed.canonical_trajectory_bytes);
  if (!trajectory.ok()) refuse_nested("prepared_motion_trajectory_invalid", trajectory.error());
  const auto participant_trajectory = EncodePreparedTrajectory(motion->trajectory);
  if (!participant_trajectory.ok() ||
      participant_trajectory.value() != sealed.canonical_trajectory_bytes) {
    refuse("prepared_motion_trajectory_mismatch");
  }

  // Key: Kernel-minted goal identity bound to this participant and binding.
  if (key.operation_id != participant.operation_id) {
    refuse("prepared_motion_key_mismatch", {{"field", "operation_id"}});
  }
  if (key.binding.active_control_configuration_generation !=
      participant.active_control_configuration_generation) {
    refuse("prepared_motion_key_mismatch", {{"field", "active_control_configuration_generation"}});
  }
  if (key.binding.provider_runtime_epoch != participant.control_provider.runtime_epoch) {
    refuse("prepared_motion_key_mismatch", {{"field", "provider_runtime_epoch"}});
  }
  if (key.controller_goal_id.empty() || !key.fence().nonzero()) {
    refuse("prepared_motion_key_mismatch", {{"field", "goal_identity"}});
  }
  const auto& binding_value = binding.value();
  if (!binding_value.control_path.has_value())
    refuse("prepared_motion_binding_without_control_path");

  // Trajectory shape.
  const auto& path = trajectory.value();
  const std::size_t joints = path.joint_names.size();
  if (joints == 0U || joints > kMaxMotionJoints) {
    refuse("prepared_motion_joint_count_invalid", {{"joint_count", std::to_string(joints)}});
  }
  if (path.waypoints.size() < 2U) refuse("prepared_motion_trajectory_too_short");
  if (!path.runtime_scale_bounds.has_value())
    refuse("prepared_motion_runtime_scale_bounds_missing");
  if (!validate_trajectory_for_sampling(path).ok())
    refuse("prepared_motion_trajectory_not_sampleable");

  // Per trajectory joint: binding resource and retained default settle.
  std::array<const RuntimeResourceBindingDTO*, kMaxMotionJoints> resource_rows{};
  std::array<double, kMaxMotionJoints> default_settle{};
  for (std::size_t j = 0U; j < joints; ++j) {
    const auto& resource = path.resources[j];
    const auto row = std::find_if(binding_value.resources.begin(), binding_value.resources.end(),
                                  [&](const auto& item) { return item.resource_id == resource; });
    if (row == binding_value.resources.end() || !row->source_joint_name.has_value()) {
      refuse("prepared_motion_resource_not_bound", {{"resource_id", resource.value()}});
    }
    resource_rows[j] = &*row;
    const auto settle =
        std::find_if(retained_default_limits.begin(), retained_default_limits.end(),
                     [&](const auto& item) { return item.resource_id == resource; });
    if (settle == retained_default_limits.end() ||
        !positive_finite(magnitude(settle->settle_velocity_threshold))) {
      refuse("prepared_motion_retained_settle_threshold_missing",
             {{"resource_id", resource.value()}});
    }
    default_settle[j] = magnitude(settle->settle_velocity_threshold);
  }

  std::vector<LabelledPlan> plans{{"ordinary", &participant.ordinary_stop_plan}};
  if (participant.fault_stop_plan) plans.push_back({"fault", &*participant.fault_stop_plan});
  for (const auto& scope : motion->interrupt_scopes) {
    plans.push_back({"interrupt_scope:" + scope.scope_id.value(), &scope.stop_plan});
  }
  const auto rest_thresholds = rest_velocity_thresholds(
      plans, std::span<const RuntimeResourceBindingDTO* const>(resource_rows.data(), joints),
      binding_value.resources, std::span<const double>(default_settle.data(), joints));

  const auto& request = motion->plan_request;
  PreparedMotionSlot slot{sealed, {}};
  auto& prepared = slot.motion;
  prepared.key = key;
  prepared.prepared_contract_digest = participant.prepared_contract_digest;
  prepared.interpolation = MotionInterpolation::kQuintic;
  prepared.joint_count = static_cast<std::uint8_t>(joints);
  prepared.runtime_scale_bounds = *path.runtime_scale_bounds;
  prepared.governor_bounds = {path.runtime_scale_bounds->max_rate_per_s,
                              path.runtime_scale_bounds->max_rate_change_per_s2};
  for (std::size_t j = 0U; j < joints; ++j) {
    const auto limit = std::find_if(
        request.per_resource_dynamics_limits.begin(), request.per_resource_dynamics_limits.end(),
        [&](const auto& item) { return item.resource_id == path.resources[j]; });
    if (limit != request.per_resource_dynamics_limits.end()) {
      prepared.effective_limits[j] = limit->limit;
    }
    prepared.final_position_tolerance_rad[j] = request.tolerances.joint_position_tol_rad;
    prepared.rest_velocity_threshold_rad_s[j] = rest_thresholds[j];
  }
  prepared.state_authority = binding_value.control_path->state_authority_class;
  prepared.trajectory = std::move(trajectory.value());
  return slot;
}

}  // namespace

Result<PreparedMotionSlot> BuildPreparedMotionSlot(
    const SealedPreparedMotionInputs& sealed, const ExecutionGoalKey& key,
    const RuntimeParticipantBinding& binding, const RuntimeParticipantStopReadiness& readiness,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category) {
  try {
    return Result<PreparedMotionSlot>::Ok(
        build(sealed, key, binding, readiness, retained_default_limits, node_category));
  } catch (const Refused& refused) {
    return Result<PreparedMotionSlot>::Err(refused.error());
  } catch (const std::exception& error) {
    return Result<PreparedMotionSlot>::Err(
        Refused("prepared_motion_build_failed", {{"what", error.what()}}).error());
  }
}

}  // namespace manyforge::core
