#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>

#include <manyforge_content_identity/canonical_value.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] Result<MotionControlledStopContractParametersDTO> invalid(
    std::string reason, std::string message) {
  return Result<MotionControlledStopContractParametersDTO>::Err(
      ErrorDTO{std::string(kErrorInvalidRequest), std::move(message),
               {{"reason", std::move(reason)}}});
}

template <typename T>
[[nodiscard]] double magnitude(const T& value) {
  return std::visit(
      [](const auto& typed) {
        if constexpr (requires { typed.value_rad_s2; }) {
          return typed.value_rad_s2;
        } else if constexpr (requires { typed.value_m_s2; }) {
          return typed.value_m_s2;
        } else if constexpr (requires { typed.value_rad_s; }) {
          return typed.value_rad_s;
        } else {
          return typed.value_m_s;
        }
      },
      value);
}

[[nodiscard]] bool finite_positive(
    const CoordinateAccelerationDTO& value) {
  const double scalar = magnitude(value);
  return std::isfinite(scalar) && scalar > 0.0;
}

[[nodiscard]] bool finite_positive(const CoordinateVelocityDTO& value) {
  const double scalar = magnitude(value);
  return std::isfinite(scalar) && scalar > 0.0;
}

[[nodiscard]] bool source_joint_name_valid(std::string_view name) {
  return content_identity::is_source_symbol(name, 1024U);
}

struct ResolvedResource final {
  const RuntimeResourceBindingDTO* binding{};
  CoordinateVelocityDTO settle_threshold;
};

}  // namespace

Result<MotionControlledStopContractParametersDTO>
map_motion_interrupt_stop_profile_to_contract_parameters(
    const MotionInterruptStopProfileDTO& profile,
    MotionInterruptNodeCategory node_category,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> retained_default_limits) {
  if (!motion_interrupt_stop_profile_id_valid(profile.profile_id)) {
    return invalid("motion_interrupt_stop_profile_id_invalid",
                   "profile_id must be a 1-128 byte portable token");
  }
  if (const auto error = motion_interrupt_stop_profile_field_error(profile);
      !error.empty()) {
    return invalid("motion_interrupt_stop_profile_invalid", error);
  }
  if (!profile.require_backend_confirmation) {
    return invalid("motion_interrupt_backend_confirmation_required",
                   "production stop profiles require backend confirmation");
  }
  if (profile.diagnostics.size() > kMaxInterruptStructuredValueEntries ||
      interrupt_structured_value_error(profile.diagnostics).has_value()) {
    return invalid("motion_interrupt_stop_profile_diagnostics_invalid",
                   "stop-profile diagnostics violate the closed bounds");
  }
  if (resources.empty() || resources.size() >
                               kMaxMotionControlledStopResourceLimits) {
    return invalid("motion_interrupt_stop_profile_resource_scope_invalid",
                   "affected resources must contain 1-256 exact rows");
  }
  if (retained_default_limits.size() != resources.size()) {
    return invalid("motion_interrupt_stop_default_coverage_mismatch",
                   "retained settle thresholds must cover the exact scope");
  }
  if (node_category == MotionInterruptNodeCategory::kPrecisionContact &&
      profile.recoverable_substitution_mode.has_value()) {
    return invalid("motion_interrupt_substitution_precision_contact_forbidden",
                   "precision-contact profiles forbid recoverable substitution");
  }

  std::map<std::string, const RuntimeResourceBindingDTO*, std::less<>>
      resource_by_source_joint;
  std::set<ResourceId> resource_ids;
  std::map<ResourceId, CoordinateVelocityDTO> default_by_resource;
  for (const auto& resource : resources) {
    if (resource.resource_id.empty() ||
        !resource_ids.insert(resource.resource_id).second ||
        !resource.source_joint_name.has_value() ||
        !source_joint_name_valid(*resource.source_joint_name) ||
        !resource_by_source_joint
             .emplace(*resource.source_joint_name, &resource)
             .second) {
      return invalid("motion_interrupt_stop_resource_binding_invalid",
                     "each affected resource needs one unique source joint");
    }
  }
  for (const auto& retained : retained_default_limits) {
    if (retained.resource_id.empty() ||
        !finite_positive(retained.settle_velocity_threshold) ||
        !default_by_resource
             .emplace(retained.resource_id,
                      retained.settle_velocity_threshold)
             .second) {
      return invalid("motion_interrupt_stop_default_limit_invalid",
                     "retained settle-threshold rows are invalid or duplicate");
    }
  }

  std::vector<ResolvedResource> resolved;
  resolved.reserve(resources.size());
  for (const auto& resource : resources) {
    const auto default_it = default_by_resource.find(resource.resource_id);
    if (default_it == default_by_resource.end() ||
        coordinate_kind(default_it->second) != resource.coordinate_kind) {
      return invalid("motion_interrupt_stop_default_coverage_mismatch",
                     "retained settle thresholds do not match exact resources");
    }
    CoordinateVelocityDTO threshold = default_it->second;
    const auto override_it =
        profile.per_joint_settle_velocity_threshold.has_value()
            ? profile.per_joint_settle_velocity_threshold->find(
                  *resource.source_joint_name)
            : std::map<std::string, CoordinateVelocityDTO>::const_iterator{};
    if (profile.per_joint_settle_velocity_threshold.has_value() &&
        override_it != profile.per_joint_settle_velocity_threshold->end()) {
      if (!finite_positive(override_it->second) ||
          coordinate_kind(override_it->second) != resource.coordinate_kind ||
          magnitude(override_it->second) > magnitude(threshold)) {
        return invalid("motion_interrupt_stop_settle_override_invalid",
                       "settle override must match units and tighten the default");
      }
      threshold = override_it->second;
    }
    resolved.push_back({&resource, std::move(threshold)});
  }
  if (profile.per_joint_settle_velocity_threshold.has_value() &&
      (profile.per_joint_settle_velocity_threshold->size() > resources.size() ||
       std::any_of(profile.per_joint_settle_velocity_threshold->begin(),
                   profile.per_joint_settle_velocity_threshold->end(),
                   [&](const auto& row) {
                     return !source_joint_name_valid(row.first) ||
                            !resource_by_source_joint.contains(row.first);
                   }))) {
    return invalid("motion_interrupt_stop_settle_override_unknown_joint",
                   "settle override references an unaffected source joint");
  }

  std::sort(resolved.begin(), resolved.end(), [](const auto& lhs,
                                                 const auto& rhs) {
    return lhs.binding->resource_id < rhs.binding->resource_id;
  });

  if (profile.stop_mode == MotionInterruptStopMode::kHardDeceleration) {
    HardDecelerationStopParametersDTO mapped;
    mapped.per_resource_limits.reserve(resolved.size());
    for (const auto& row : resolved) {
      mapped.per_resource_limits.push_back(
          {row.binding->resource_id, row.settle_threshold});
    }
    return Result<MotionControlledStopContractParametersDTO>::Ok(
        MotionControlledStopContractParametersDTO{std::move(mapped)});
  }

  if (profile.per_joint_max_deceleration.size() != resources.size()) {
    return invalid("motion_interrupt_stop_deceleration_coverage_mismatch",
                   "per-joint acceleration limits must exactly cover scope");
  }
  std::map<ResourceId, CoordinateAccelerationDTO> acceleration_by_resource;
  for (const auto& [source_joint, acceleration] :
       profile.per_joint_max_deceleration) {
    const auto resource_it = resource_by_source_joint.find(source_joint);
    if (!source_joint_name_valid(source_joint) ||
        resource_it == resource_by_source_joint.end() ||
        !finite_positive(acceleration) ||
        coordinate_kind(acceleration) !=
            resource_it->second->coordinate_kind) {
      return invalid("motion_interrupt_stop_deceleration_limit_invalid",
                     "acceleration limits must match exact source-joint units");
    }
    acceleration_by_resource.emplace(resource_it->second->resource_id,
                                     acceleration);
  }
  if (acceleration_by_resource.size() != resources.size()) {
    return invalid("motion_interrupt_stop_deceleration_coverage_mismatch",
                   "per-joint acceleration limits must exactly cover scope");
  }

  std::vector<ControlledStopResourceLimitDTO> limits;
  limits.reserve(resolved.size());
  for (const auto& row : resolved) {
    limits.push_back({row.binding->resource_id,
                      acceleration_by_resource.at(row.binding->resource_id),
                      row.settle_threshold});
  }
  if (profile.stop_mode == MotionInterruptStopMode::kSoftOnPath) {
    SoftOnPathStopParametersDTO mapped;
    mapped.deceleration_time_s = *profile.deceleration_time_s;
    mapped.per_resource_limits = std::move(limits);
    mapped.allow_natural_completion_on_short_path =
        profile.allow_natural_completion_on_short_path;
    if (profile.recoverable_substitution_mode.has_value()) {
      mapped.recoverable_substitution_mode =
          MotionControlledStopMode::kSpringBackHold;
    }
    return Result<MotionControlledStopContractParametersDTO>::Ok(
        MotionControlledStopContractParametersDTO{std::move(mapped)});
  }

  SpringBackHoldStopParametersDTO mapped;
  mapped.deceleration_time_s =
      motion_interrupt_deceleration_time_or_default(profile);
  mapped.per_resource_limits = std::move(limits);
  return Result<MotionControlledStopContractParametersDTO>::Ok(
      MotionControlledStopContractParametersDTO{std::move(mapped)});
}

}  // namespace manyforge::core
