#include <algorithm>
#include <cmath>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] Result<MotionControlledStopContractParametersDTO> invalid(std::string reason,
                                                                        std::string message) {
  return Result<MotionControlledStopContractParametersDTO>::Err(ErrorDTO{
      std::string(kErrorInvalidRequest), std::move(message), {{"reason", std::move(reason)}}});
}

[[nodiscard]] Result<MotionInterruptStopProfileDTO> invalid_profile(std::string reason,
                                                                    std::string message) {
  return Result<MotionInterruptStopProfileDTO>::Err(ErrorDTO{
      std::string(kErrorInvalidRequest), std::move(message), {{"reason", std::move(reason)}}});
}

[[nodiscard]] const JsonValueDTO* member(const JsonObjectDTO& object, std::string_view name) {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] const JsonObjectDTO* object(const JsonValueDTO* value) {
  return value == nullptr ? nullptr : std::get_if<JsonObjectDTO>(&value->value);
}

[[nodiscard]] const JsonValueDTO::Array* array(const JsonValueDTO* value) {
  return value == nullptr ? nullptr : std::get_if<JsonValueDTO::Array>(&value->value);
}

[[nodiscard]] const std::string* text(const JsonValueDTO* value) {
  return value == nullptr ? nullptr : std::get_if<std::string>(&value->value);
}

[[nodiscard]] const double* number(const JsonValueDTO* value) {
  return value == nullptr ? nullptr : std::get_if<double>(&value->value);
}

[[nodiscard]] const bool* boolean(const JsonValueDTO* value) {
  return value == nullptr ? nullptr : std::get_if<bool>(&value->value);
}

[[nodiscard]] std::optional<CoordinateAccelerationDTO> acceleration(const JsonObjectDTO& value,
                                                                    CoordinateKind expected_kind) {
  const auto* kind = text(member(value, "coordinate_kind"));
  if (expected_kind == CoordinateKind::kAngular) {
    const auto* magnitude = number(member(value, "value_rad_s2"));
    if (kind == nullptr || *kind != "angular_rad_s2" || magnitude == nullptr ||
        !std::isfinite(*magnitude) || *magnitude <= 0.0) {
      return std::nullopt;
    }
    return CoordinateAccelerationDTO{AngularAccelerationDTO{*magnitude}};
  }
  const auto* magnitude = number(member(value, "value_m_s2"));
  if (kind == nullptr || *kind != "linear_m_s2" || magnitude == nullptr ||
      !std::isfinite(*magnitude) || *magnitude <= 0.0) {
    return std::nullopt;
  }
  return CoordinateAccelerationDTO{LinearAccelerationDTO{*magnitude}};
}

[[nodiscard]] std::optional<CoordinateVelocityDTO> velocity(const JsonObjectDTO& value,
                                                            CoordinateKind expected_kind) {
  const auto* kind = text(member(value, "coordinate_kind"));
  if (expected_kind == CoordinateKind::kAngular) {
    const auto* magnitude = number(member(value, "value_rad_s"));
    if (kind == nullptr || *kind != "angular_rad_s" || magnitude == nullptr ||
        !std::isfinite(*magnitude) || *magnitude <= 0.0) {
      return std::nullopt;
    }
    return CoordinateVelocityDTO{AngularVelocityDTO{*magnitude}};
  }
  const auto* magnitude = number(member(value, "value_m_s"));
  if (kind == nullptr || *kind != "linear_m_s" || magnitude == nullptr ||
      !std::isfinite(*magnitude) || *magnitude <= 0.0) {
    return std::nullopt;
  }
  return CoordinateVelocityDTO{LinearVelocityDTO{*magnitude}};
}

[[nodiscard]] std::optional<CoordinateJerkDTO> jerk(const JsonObjectDTO& value,
                                                    CoordinateKind expected_kind) {
  const bool angular = expected_kind == CoordinateKind::kAngular;
  const auto* kind = text(member(value, "coordinate_kind"));
  const auto* scalar = number(member(value, angular ? "value_rad_s3" : "value_m_s3"));
  if (value.size() != 2U || kind == nullptr ||
      *kind != (angular ? "angular_rad_s3" : "linear_m_s3") || scalar == nullptr ||
      !std::isfinite(*scalar) || *scalar <= 0.0)
    return std::nullopt;
  if (angular) return CoordinateJerkDTO{AngularJerkDTO{*scalar}};
  return CoordinateJerkDTO{LinearJerkDTO{*scalar}};
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

[[nodiscard]] bool finite_positive(const CoordinateAccelerationDTO& value) {
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

Result<MotionInterruptStopProfileDTO> resolve_motion_interrupt_stop_jerk_limits(
    const MotionInterruptStopProfileDTO& source) {
  if (const auto error = motion_interrupt_stop_profile_field_error(source); !error.empty())
    return invalid_profile("motion_interrupt_stop_profile_invalid", error);
  auto profile = source;
  if (!profile.jerk_limits_enabled.has_value() &&
      profile.stop_mode != MotionInterruptStopMode::kHardDeceleration)
    return invalid_profile("motion_controlled_stop_jerk_activation_required",
                           "Review and save an explicit controlled-Stop jerk choice");
  if (profile.stop_mode == MotionInterruptStopMode::kHardDeceleration)
    profile.jerk_limits_enabled = false;
  if (profile.jerk_limits_enabled != true)
    return Result<MotionInterruptStopProfileDTO>::Ok(std::move(profile));
  const double reference_s =
      motion_interrupt_deceleration_time_or_default(profile) *
      (profile.stop_mode == MotionInterruptStopMode::kSpringBackHold ? 0.5 : 1.0);
  for (const auto& [joint, acceleration] : profile.per_joint_max_deceleration) {
    if (profile.per_joint_max_jerk.contains(joint)) continue;
    const double limit = motion_jerk_preset_limit(magnitude(acceleration), reference_s);
    if (!std::isfinite(limit) || limit <= 0.0)
      return invalid_profile("motion_interrupt_jerk_preset_invalid",
                             "Jerk preset is not finite and positive");
    profile.per_joint_max_jerk.emplace(joint,
                                       coordinate_kind(acceleration) == CoordinateKind::kAngular
                                           ? CoordinateJerkDTO{AngularJerkDTO{limit}}
                                           : CoordinateJerkDTO{LinearJerkDTO{limit}});
  }
  return Result<MotionInterruptStopProfileDTO>::Ok(std::move(profile));
}

Result<MotionControlledStopContractParametersDTO>
map_motion_interrupt_stop_profile_to_contract_parameters(
    const MotionInterruptStopProfileDTO& source, MotionInterruptNodeCategory node_category,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> retained_default_limits) {
  if (!motion_interrupt_stop_profile_id_valid(source.profile_id))
    return invalid("motion_interrupt_stop_profile_id_invalid",
                   "profile_id must be a 1-128 byte portable token");
  const auto frozen = resolve_motion_interrupt_stop_jerk_limits(source);
  if (!frozen.ok()) return Result<MotionControlledStopContractParametersDTO>::Err(frozen.error());
  const auto& profile = frozen.value();
  if (!motion_interrupt_stop_profile_id_valid(profile.profile_id)) {
    return invalid("motion_interrupt_stop_profile_id_invalid",
                   "profile_id must be a 1-128 byte portable token");
  }
  if (const auto error = motion_interrupt_stop_profile_field_error(profile); !error.empty()) {
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
  if (resources.empty() || resources.size() > kMaxMotionControlledStopResourceLimits) {
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
  if (node_category == MotionInterruptNodeCategory::kPrecisionContact &&
      profile.allow_natural_completion_on_short_path) {
    return invalid("motion_interrupt_natural_completion_precision_contact_forbidden",
                   "precision-contact profiles require a controlled stop, not natural completion");
  }

  std::map<std::string, const RuntimeResourceBindingDTO*, std::less<>> resource_by_source_joint;
  std::set<ResourceId> resource_ids;
  std::map<ResourceId, CoordinateVelocityDTO> default_by_resource;
  for (const auto& resource : resources) {
    if (resource.resource_id.empty() || !resource_ids.insert(resource.resource_id).second ||
        !resource.source_joint_name.has_value() ||
        !source_joint_name_valid(*resource.source_joint_name) ||
        !resource_by_source_joint.emplace(*resource.source_joint_name, &resource).second) {
      return invalid("motion_interrupt_stop_resource_binding_invalid",
                     "each affected resource needs one unique source joint");
    }
  }
  for (const auto& retained : retained_default_limits) {
    if (retained.resource_id.empty() || !finite_positive(retained.settle_velocity_threshold) ||
        !default_by_resource.emplace(retained.resource_id, retained.settle_velocity_threshold)
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
            ? profile.per_joint_settle_velocity_threshold->find(*resource.source_joint_name)
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
                   profile.per_joint_settle_velocity_threshold->end(), [&](const auto& row) {
                     return !source_joint_name_valid(row.first) ||
                            !resource_by_source_joint.contains(row.first);
                   }))) {
    return invalid("motion_interrupt_stop_settle_override_unknown_joint",
                   "settle override references an unaffected source joint");
  }

  std::sort(resolved.begin(), resolved.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.binding->resource_id < rhs.binding->resource_id;
  });

  if (profile.stop_mode == MotionInterruptStopMode::kHardDeceleration) {
    HardDecelerationStopParametersDTO mapped;
    mapped.per_resource_limits.reserve(resolved.size());
    for (const auto& row : resolved) {
      mapped.per_resource_limits.push_back({row.binding->resource_id, row.settle_threshold});
    }
    return Result<MotionControlledStopContractParametersDTO>::Ok(
        MotionControlledStopContractParametersDTO{std::move(mapped)});
  }

  if (profile.per_joint_max_deceleration.size() != resources.size()) {
    return invalid("motion_interrupt_stop_deceleration_coverage_mismatch",
                   "per-joint acceleration limits must exactly cover scope");
  }
  std::map<ResourceId, CoordinateAccelerationDTO> acceleration_by_resource;
  for (const auto& [source_joint, acceleration] : profile.per_joint_max_deceleration) {
    const auto resource_it = resource_by_source_joint.find(source_joint);
    if (!source_joint_name_valid(source_joint) || resource_it == resource_by_source_joint.end() ||
        !finite_positive(acceleration) ||
        coordinate_kind(acceleration) != resource_it->second->coordinate_kind) {
      return invalid("motion_interrupt_stop_deceleration_limit_invalid",
                     "acceleration limits must match exact source-joint units");
    }
    acceleration_by_resource.emplace(resource_it->second->resource_id, acceleration);
  }
  if (acceleration_by_resource.size() != resources.size()) {
    return invalid("motion_interrupt_stop_deceleration_coverage_mismatch",
                   "per-joint acceleration limits must exactly cover scope");
  }

  std::vector<ControlledStopResourceLimitDTO> limits;
  limits.reserve(resolved.size());
  for (const auto& row : resolved) {
    const auto jerk = profile.per_joint_max_jerk.find(*row.binding->source_joint_name);
    limits.push_back(
        {row.binding->resource_id, acceleration_by_resource.at(row.binding->resource_id),
         row.settle_threshold,
         jerk == profile.per_joint_max_jerk.end() ? std::nullopt : std::optional(jerk->second)});
  }
  if (profile.stop_mode == MotionInterruptStopMode::kSoftOnPath) {
    SoftOnPathStopParametersDTO mapped;
    mapped.deceleration_time_s = *profile.deceleration_time_s;
    mapped.per_resource_limits = std::move(limits);
    mapped.allow_natural_completion_on_short_path = profile.allow_natural_completion_on_short_path;
    mapped.jerk_limits_enabled = profile.jerk_limits_enabled == true;
    if (profile.recoverable_substitution_mode.has_value()) {
      mapped.recoverable_substitution_mode = MotionControlledStopMode::kSpringBackHold;
    }
    return Result<MotionControlledStopContractParametersDTO>::Ok(
        MotionControlledStopContractParametersDTO{std::move(mapped)});
  }

  SpringBackHoldStopParametersDTO mapped;
  mapped.deceleration_time_s = motion_interrupt_deceleration_time_or_default(profile);
  mapped.per_resource_limits = std::move(limits);
  mapped.allow_natural_completion_on_short_path = profile.allow_natural_completion_on_short_path;
  mapped.jerk_limits_enabled = profile.jerk_limits_enabled == true;
  return Result<MotionControlledStopContractParametersDTO>::Ok(
      MotionControlledStopContractParametersDTO{std::move(mapped)});
}

Result<MotionInterruptStopProfileDTO> map_motion_controlled_stop_contract_parameters_to_profile(
    const JsonObjectDTO& parameters, std::span<const RuntimeResourceBindingDTO> resources,
    std::string_view profile_id) {
  if (!motion_interrupt_stop_profile_id_valid(profile_id)) {
    return invalid_profile("motion_controlled_stop_profile_id_invalid",
                           "controlled Stop profile identity is invalid");
  }
  if (resources.empty() || resources.size() > kMaxMotionControlledStopResourceLimits) {
    return invalid_profile("motion_controlled_stop_resource_scope_invalid",
                           "controlled Stop needs one exact resource scope");
  }

  std::map<ResourceId, const RuntimeResourceBindingDTO*> by_resource;
  for (const auto& resource : resources) {
    if (resource.resource_id.empty() || !resource.source_joint_name.has_value() ||
        !source_joint_name_valid(*resource.source_joint_name) ||
        !by_resource.emplace(resource.resource_id, &resource).second) {
      return invalid_profile("motion_controlled_stop_resource_binding_invalid",
                             "controlled Stop resources are not exact joints");
    }
  }

  const auto* mode = text(member(parameters, "stop_mode"));
  const auto* limits = array(member(parameters, "per_resource_limits"));
  if (mode == nullptr || limits == nullptr || limits->size() != resources.size()) {
    return invalid_profile("motion_controlled_stop_parameters_invalid",
                           "controlled Stop parameters do not cover the scope");
  }

  MotionInterruptStopProfileDTO profile;
  profile.profile_id = std::string(profile_id);
  profile.require_backend_confirmation = true;
  if (*mode == "soft_on_path") {
    profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  } else if (*mode == "spring_back_hold") {
    profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  } else if (*mode == "hard_deceleration") {
    profile.stop_mode = MotionInterruptStopMode::kHardDeceleration;
    profile.jerk_limits_enabled = false;
  } else {
    return invalid_profile("motion_controlled_stop_mode_invalid",
                           "controlled Stop mode is not supported");
  }

  if (profile.stop_mode != MotionInterruptStopMode::kHardDeceleration) {
    const auto* enabled = boolean(member(parameters, "jerk_limits_enabled"));
    if (enabled == nullptr)
      return invalid_profile("motion_controlled_stop_jerk_activation_required",
                             "Review and save an explicit controlled-Stop jerk choice");
    profile.jerk_limits_enabled = *enabled;
    const auto* duration = number(member(parameters, "deceleration_time_s"));
    if (duration == nullptr || !std::isfinite(*duration) || *duration <= 0.0) {
      return invalid_profile("motion_controlled_stop_duration_invalid",
                             "controlled Stop duration is invalid");
    }
    profile.deceleration_time_s = *duration;
  }
  if (profile.stop_mode != MotionInterruptStopMode::kHardDeceleration) {
    const auto* natural = boolean(member(parameters, "allow_natural_completion_on_short_path"));
    if (natural == nullptr) {
      return invalid_profile("motion_controlled_stop_natural_completion_invalid",
                             "Controlled Stop natural-completion policy is absent");
    }
    profile.allow_natural_completion_on_short_path = *natural;
    if (const auto* substitute = text(member(parameters, "recoverable_substitution_mode"));
        substitute != nullptr) {
      if (profile.stop_mode != MotionInterruptStopMode::kSoftOnPath ||
          *substitute != "spring_back_hold") {
        return invalid_profile("motion_controlled_stop_substitution_invalid",
                               "controlled Stop substitution is invalid");
      }
      profile.recoverable_substitution_mode = MotionInterruptStopMode::kSpringBackHold;
    }
  }

  std::set<ResourceId> seen;
  std::map<std::string, CoordinateVelocityDTO> settle_thresholds;
  for (const auto& encoded : *limits) {
    const auto* row = std::get_if<JsonObjectDTO>(&encoded.value);
    const auto* resource_id = row == nullptr ? nullptr : text(member(*row, "resource_id"));
    if (resource_id == nullptr) {
      return invalid_profile("motion_controlled_stop_resource_limit_invalid",
                             "controlled Stop has an invalid resource row");
    }
    const ResourceId identity(*resource_id);
    const auto expected = by_resource.find(identity);
    if (expected == by_resource.end() || !seen.insert(identity).second) {
      return invalid_profile("motion_controlled_stop_resource_limit_invalid",
                             "controlled Stop resource rows differ from scope");
    }
    const auto* settle = object(member(*row, "settle_velocity_threshold"));
    const auto decoded_settle = settle == nullptr
                                    ? std::optional<CoordinateVelocityDTO>{}
                                    : velocity(*settle, expected->second->coordinate_kind);
    if (!decoded_settle.has_value()) {
      return invalid_profile("motion_controlled_stop_settle_limit_invalid",
                             "controlled Stop settle threshold is invalid");
    }
    settle_thresholds.emplace(*expected->second->source_joint_name, *decoded_settle);
    if (profile.stop_mode != MotionInterruptStopMode::kHardDeceleration) {
      const auto* deceleration = object(member(*row, "max_deceleration"));
      const auto decoded_deceleration =
          deceleration == nullptr ? std::optional<CoordinateAccelerationDTO>{}
                                  : acceleration(*deceleration, expected->second->coordinate_kind);
      if (!decoded_deceleration.has_value()) {
        return invalid_profile("motion_controlled_stop_deceleration_invalid",
                               "controlled Stop deceleration limit is invalid");
      }
      profile.per_joint_max_deceleration.emplace(*expected->second->source_joint_name,
                                                 *decoded_deceleration);
      if (const auto* encoded_jerk = member(*row, "max_jerk")) {
        const auto* value = object(encoded_jerk);
        const auto decoded = value == nullptr ? std::optional<CoordinateJerkDTO>{}
                                              : jerk(*value, expected->second->coordinate_kind);
        if (!decoded)
          return invalid_profile("motion_controlled_stop_jerk_limit_invalid",
                                 "Jerk must be positive and match the exact resource units");
        profile.per_joint_max_jerk.emplace(*expected->second->source_joint_name, *decoded);
      }
    }
  }
  profile.per_joint_settle_velocity_threshold = std::move(settle_thresholds);
  if (const auto error = motion_interrupt_stop_profile_field_error(profile); !error.empty()) {
    return invalid_profile("motion_controlled_stop_profile_invalid", error);
  }
  return Result<MotionInterruptStopProfileDTO>::Ok(std::move(profile));
}

Result<MotionInterruptStopProfileDTO> project_motion_interrupt_stop_profile_to_compiled_resources(
    const MotionInterruptStopProfileDTO& profile,
    std::span<const RuntimeResourceBindingDTO> resources) {
  if (const auto error = motion_interrupt_stop_profile_field_error(profile); !error.empty()) {
    return invalid_profile("motion_interrupt_stop_profile_invalid", error);
  }
  if (resources.empty() || resources.size() > kMaxMotionControlledStopResourceLimits) {
    return invalid_profile("motion_controlled_stop_resource_scope_invalid",
                           "controlled Stop needs one exact resource scope");
  }

  std::map<std::string, std::string> compiled_by_source;
  std::set<std::string> compiled_names;
  for (const auto& resource : resources) {
    if (!resource.source_joint_name.has_value() ||
        !source_joint_name_valid(*resource.source_joint_name) ||
        resource.compiled_resource_name.empty() ||
        !compiled_by_source.emplace(*resource.source_joint_name, resource.compiled_resource_name)
             .second ||
        !compiled_names.emplace(resource.compiled_resource_name).second) {
      return invalid_profile("motion_controlled_stop_joint_projection_invalid",
                             "controlled Stop source and compiled joints are not one-to-one");
    }
  }

  MotionInterruptStopProfileDTO projected = profile;
  projected.per_joint_max_deceleration.clear();
  projected.per_joint_max_jerk.clear();
  for (const auto& [source_joint, limit] : profile.per_joint_max_deceleration) {
    const auto compiled = compiled_by_source.find(source_joint);
    if (compiled == compiled_by_source.end()) {
      return invalid_profile("motion_controlled_stop_joint_projection_mismatch",
                             "controlled Stop acceleration scope differs from its retained "
                             "joint projection");
    }
    projected.per_joint_max_deceleration.emplace(compiled->second, limit);
  }
  for (const auto& [source_joint, limit] : profile.per_joint_max_jerk) {
    const auto compiled = compiled_by_source.find(source_joint);
    if (compiled == compiled_by_source.end())
      return invalid_profile(
          "motion_controlled_stop_joint_projection_mismatch",
          "Controlled Stop jerk scope differs from the retained joint projection");
    projected.per_joint_max_jerk.emplace(compiled->second, limit);
  }

  if (profile.per_joint_settle_velocity_threshold.has_value()) {
    std::map<std::string, CoordinateVelocityDTO> thresholds;
    for (const auto& [source_joint, threshold] : *profile.per_joint_settle_velocity_threshold) {
      const auto compiled = compiled_by_source.find(source_joint);
      if (compiled == compiled_by_source.end()) {
        return invalid_profile("motion_controlled_stop_joint_projection_mismatch",
                               "controlled Stop settle scope differs from its retained joint "
                               "projection");
      }
      thresholds.emplace(compiled->second, threshold);
    }
    projected.per_joint_settle_velocity_threshold = std::move(thresholds);
  }

  if (const auto error = motion_interrupt_stop_profile_field_error(projected); !error.empty()) {
    return invalid_profile("motion_controlled_stop_projection_invalid", error);
  }
  return Result<MotionInterruptStopProfileDTO>::Ok(std::move(projected));
}

JsonValueDTO motion_controlled_stop_evidence(const MotionInterruptStopProfileDTO& requested_profile,
                                             const ControlledStopResultDTO& result,
                                             const std::string_view stop_evidence_policy_revision,
                                             const std::string_view parameter_source) {
  JsonObjectDTO evidence{
      {"profile_id", JsonValueDTO{requested_profile.profile_id}},
      {"profile_source", JsonValueDTO{std::string(parameter_source)}},
      {"stop_evidence_policy_revision", JsonValueDTO{std::string(stop_evidence_policy_revision)}},
      {"requested_stop_mode",
       JsonValueDTO{std::string(stop_mode_string(requested_profile.stop_mode))}},
      {"actual_stop_mode", JsonValueDTO{std::string(stop_mode_string(result.executed_stop_mode))}},
      {"completed_by_natural_completion", JsonValueDTO{result.completed_by_natural_completion}},
      {"stop_command_source",
       JsonValueDTO{std::string(stop_command_source_string(result.stop_command_source))}},
      {"backend_stop_confirmed", JsonValueDTO{result.backend_stop_confirmed}},
      {"confirmation_latency_ns",
       JsonValueDTO{static_cast<double>(result.confirmation_latency_ns)}},
  };
  if (result.path_deviation_max_rad.has_value()) {
    evidence.emplace("path_deviation_max_rad", JsonValueDTO{*result.path_deviation_max_rad});
  }
  if (result.initial_path_deviation_rad.has_value()) {
    evidence.emplace("initial_path_deviation_rad",
                     JsonValueDTO{*result.initial_path_deviation_rad});
  }
  if (result.stop_distance_rad.has_value()) {
    evidence.emplace("stop_distance_rad", JsonValueDTO{*result.stop_distance_rad});
  }
  if (result.fallback_reason.has_value()) {
    evidence.emplace("fallback_reason", JsonValueDTO{*result.fallback_reason});
  }
  if (!result.diagnostics.empty()) {
    JsonObjectDTO diagnostics;
    for (const auto& [name, value] : result.diagnostics) {
      diagnostics.emplace(name,
                          std::visit([](const auto& typed) { return JsonValueDTO{typed}; }, value));
    }
    evidence.emplace("provider_diagnostics", JsonValueDTO{std::move(diagnostics)});
  }
  return JsonValueDTO{std::move(evidence)};
}

}  // namespace manyforge::core
