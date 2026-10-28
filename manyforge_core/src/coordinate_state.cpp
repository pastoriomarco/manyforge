#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_core/time/received_sample_age.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] ErrorDTO invalid(std::string reason) {
  return ErrorDTO{"invalid_request", "coordinate state is invalid",
                  {{"reason", std::move(reason)}}};
}

[[nodiscard]] bool finite(const CoordinatePositionDTO& value) {
  return std::visit(
      [](const auto& item) {
        if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                     AngularPositionDTO>) {
          return std::isfinite(item.value_rad);
        } else {
          return std::isfinite(item.value_m);
        }
      },
      value);
}

[[nodiscard]] bool finite_velocity(const CoordinateVelocityDTO& value) {
  return std::visit(
      [](const auto& item) {
        if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                     AngularVelocityDTO>) {
          return std::isfinite(item.value_rad_s);
        } else {
          return std::isfinite(item.value_m_s);
        }
      },
      value);
}

template <typename State>
[[nodiscard]] std::optional<ErrorDTO> validate_common(const State& state) {
  if (state.controllable_component_id.empty() ||
      state.command_target_id.empty() || state.control_domain_id.empty() ||
      state.control_domain_revision.empty() ||
      state.control_binding_id.empty() ||
      state.control_binding_revision.empty() ||
      state.controller_topology_id.empty() || state.topology_revision.empty() ||
      state.control_route_id.empty() ||
      state.active_control_configuration_id.empty() ||
      !state.active_control_configuration_generation.nonzero()) {
    return invalid("addressed_identity_missing");
  }
  if (state.resources.empty() || state.resources.size() != state.joint_names.size() ||
      state.resources.size() != state.positions.size() ||
      (state.velocities.has_value() &&
       state.velocities->size() != state.resources.size())) {
    return invalid("coordinate_dimension_mismatch");
  }
  std::set<std::string> resources;
  std::set<std::string> names;
  for (std::size_t index = 0U; index < state.resources.size(); ++index) {
    if (state.resources[index].empty() || state.joint_names[index].empty() ||
        !resources.insert(state.resources[index].value()).second ||
        !names.insert(state.joint_names[index]).second) {
      return invalid("coordinate_identity_missing_or_duplicate");
    }
    if (!finite(state.positions[index])) {
      return invalid("coordinate_position_nonfinite");
    }
    if (state.velocities.has_value()) {
      const auto& velocity = (*state.velocities)[index];
      if (!finite_velocity(velocity)) {
        return invalid("coordinate_velocity_nonfinite");
      }
      if (coordinate_kind(state.positions[index]) != coordinate_kind(velocity)) {
        return invalid("coordinate_unit_mismatch");
      }
    }
  }
  return std::nullopt;
}

}  // namespace

Result<void> validate_controlled_state(const ControlledStateDTO& state) {
  if (auto error = validate_common(state); error.has_value()) {
    return Result<void>::Err(std::move(*error));
  }
  return Result<void>::Ok();
}

Result<void> validate_observed_state_snapshot(
    const ObservedStateSnapshotDTO& state, std::int64_t now_ns) {
  if (auto error = validate_common(state); error.has_value()) {
    return Result<void>::Err(std::move(*error));
  }
  if (now_ns < 0 || state.sequence == 0U || state.source_id.empty() ||
      state.source_epoch.empty() || state.state_authority_id.empty() ||
      state.authority_generation == 0U || state.received_timestamp_ns < 0 ||
      state.freshness_budget_ns < 0) {
    return Result<void>::Err(invalid("observation_metadata_invalid"));
  }
  if (state.state_authority_class == StateAuthorityClass::kMeasured &&
      state.freshness_budget_ns <= 0) {
    return Result<void>::Err(
        invalid("measured_freshness_budget_required"));
  }
  if (state.freshness_budget_ns > 0 &&
      !received_sample_current(state.received_timestamp_ns, now_ns,
                               state.freshness_budget_ns)) {
    return Result<void>::Err(invalid("observation_stale"));
  }
  if (state.state_authority_class == StateAuthorityClass::kMeasured &&
      (state.source_kind == ObservedStateSourceKind::kCommandedState ||
       state.source_kind == ObservedStateSourceKind::kPredictedState)) {
    return Result<void>::Err(invalid("source_authority_class_mismatch"));
  }
  if (state.state_authority_class == StateAuthorityClass::kCommanded &&
      state.source_kind != ObservedStateSourceKind::kCommandedState) {
    return Result<void>::Err(invalid("source_authority_class_mismatch"));
  }
  if (state.state_authority_class == StateAuthorityClass::kPredicted &&
      state.source_kind != ObservedStateSourceKind::kPredictedState) {
    return Result<void>::Err(invalid("source_authority_class_mismatch"));
  }
  if (!state.complete) {
    return Result<void>::Err(invalid("observation_incomplete"));
  }
  return Result<void>::Ok();
}

Result<std::vector<double>> angular_position_projection(
    const ControlledStateDTO& state) {
  if (auto validation = validate_controlled_state(state); !validation.ok()) {
    return Result<std::vector<double>>::Err(validation.error());
  }
  std::vector<double> positions;
  positions.reserve(state.positions.size());
  for (const auto& position : state.positions) {
    const auto* angular = std::get_if<AngularPositionDTO>(&position);
    if (angular == nullptr) {
      return Result<std::vector<double>>::Err(
          invalid("angular_projection_requires_all_angular_resources"));
    }
    positions.push_back(angular->value_rad);
  }
  return Result<std::vector<double>>::Ok(std::move(positions));
}

}  // namespace manyforge::core
