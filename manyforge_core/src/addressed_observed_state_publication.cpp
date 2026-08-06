#include <manyforge_core/addressed_observed_state_publication.hpp>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] ErrorDTO state_error(std::string reason) {
  return ErrorDTO{"invalid_request", "addressed observed state is unavailable",
                  {{"reason", std::move(reason)}}};
}

[[nodiscard]] ObservedRobotStateAuthorityProfile authority_profile(
    StateAuthorityClass authority) {
  switch (authority) {
    case StateAuthorityClass::kMeasured:
      return ObservedRobotStateAuthorityProfile::kMeasured;
    case StateAuthorityClass::kCommanded:
      return ObservedRobotStateAuthorityProfile::kCommandedSimulation;
    case StateAuthorityClass::kPredicted:
      return ObservedRobotStateAuthorityProfile::kPredictedNative;
  }
  throw std::invalid_argument("unknown state-authority class");
}

[[nodiscard]] bool source_kind_matches_authority(
    ObservedRobotStateSourceKind source,
    StateAuthorityClass authority) noexcept {
  switch (authority) {
    case StateAuthorityClass::kMeasured:
      return source == ObservedRobotStateSourceKind::kActionFeedback ||
             source == ObservedRobotStateSourceKind::kControllerState ||
             source == ObservedRobotStateSourceKind::kJointState;
    case StateAuthorityClass::kCommanded:
      return source == ObservedRobotStateSourceKind::kCommandedState;
    case StateAuthorityClass::kPredicted:
      return source == ObservedRobotStateSourceKind::kPredictedState;
  }
  return false;
}

[[nodiscard]] ObservedRobotStateSourceKind numeric_source_kind(
    ObservedStateSourceKind source) noexcept {
  switch (source) {
    case ObservedStateSourceKind::kActionFeedback:
      return ObservedRobotStateSourceKind::kActionFeedback;
    case ObservedStateSourceKind::kControllerState:
      return ObservedRobotStateSourceKind::kControllerState;
    case ObservedStateSourceKind::kJointState:
      return ObservedRobotStateSourceKind::kJointState;
    case ObservedStateSourceKind::kCommandedState:
      return ObservedRobotStateSourceKind::kCommandedState;
    case ObservedStateSourceKind::kPredictedState:
      return ObservedRobotStateSourceKind::kPredictedState;
  }
  return ObservedRobotStateSourceKind::kJointState;
}

[[nodiscard]] ObservedStateSourceKind addressed_source_kind(
    ObservedRobotStateSourceKind source) noexcept {
  switch (source) {
    case ObservedRobotStateSourceKind::kActionFeedback:
      return ObservedStateSourceKind::kActionFeedback;
    case ObservedRobotStateSourceKind::kControllerState:
      return ObservedStateSourceKind::kControllerState;
    case ObservedRobotStateSourceKind::kJointState:
      return ObservedStateSourceKind::kJointState;
    case ObservedRobotStateSourceKind::kCommandedState:
      return ObservedStateSourceKind::kCommandedState;
    case ObservedRobotStateSourceKind::kPredictedState:
      return ObservedStateSourceKind::kPredictedState;
  }
  return ObservedStateSourceKind::kJointState;
}

void validate_config(const AddressedObservedStatePublicationConfig& config) {
  const auto& context = config.query_context;
  if (context.controllable_component_id.empty() ||
      context.command_target_id.empty() || context.capability_id.empty() ||
      context.control_domain_id.empty() ||
      context.control_domain_revision.empty() ||
      context.control_binding_id.empty() ||
      context.control_binding_revision.empty() ||
      context.controller_topology_id.empty() ||
      context.topology_revision.empty() || context.control_route_id.empty() ||
      context.control_interface_contract_id.empty() ||
      context.control_interface_revision.empty() ||
      context.active_control_configuration_id.empty() ||
      !context.active_control_configuration_generation.nonzero() ||
      context.provider_instance_id.empty() ||
      context.provider_manifest_id.empty() ||
      context.provider_manifest_revision.empty() ||
      context.runtime_epoch.empty() || context.state_authority_id.empty() ||
      config.authority_generation == 0U) {
    throw std::invalid_argument(
        "addressed observed-state publication has incomplete identity");
  }
  if (context.resources.empty() ||
      context.resources.size() != context.joint_names.size() ||
      context.resources.size() != context.coordinate_kinds.size() ||
      context.resources.size() > config.maximum_source_resource_count) {
    throw std::invalid_argument(
        "addressed observed-state publication has invalid resource shape");
  }
  std::set<ResourceId> resources;
  std::set<std::string> names;
  for (std::size_t index = 0U; index < context.resources.size(); ++index) {
    if (context.resources[index].empty() || context.joint_names[index].empty() ||
        !resources.insert(context.resources[index]).second ||
        !names.insert(context.joint_names[index]).second) {
      throw std::invalid_argument(
          "addressed observed-state resources must be nonempty and unique");
    }
  }
  if (context.freshness_budget_ns < 0 ||
      (context.state_authority_class == StateAuthorityClass::kMeasured &&
       context.freshness_budget_ns <= 0)) {
    throw std::invalid_argument(
        "measured addressed state requires a positive freshness budget");
  }
  if (config.source_precedence.empty()) {
    throw std::invalid_argument(
        "addressed observed-state publication requires a source");
  }
  const auto expected_profile = authority_profile(context.state_authority_class);
  for (const auto& source : config.source_precedence) {
    if (source.source_id.empty() ||
        source.authority_profile != expected_profile ||
        source.freshness_budget_ns != context.freshness_budget_ns ||
        !source_kind_matches_authority(source.kind,
                                       context.state_authority_class)) {
      throw std::invalid_argument(
          "observed-state source disagrees with the retained authority");
    }
  }
}

}  // namespace

AddressedObservedStatePublication::AddressedObservedStatePublication(
    AddressedObservedStatePublicationConfig config)
    : config_([&config]() {
        validate_config(config);
        return std::move(config);
      }()),
      publication_(config_.query_context.joint_names,
                   config_.source_precedence,
                   config_.maximum_source_resource_count) {}

ObservedRobotStatePublishResult AddressedObservedStatePublication::publish(
    ObservedStateSourceKind source,
    std::span<const std::string> source_joint_names,
    std::span<const double> positions_si,
    std::span<const double> velocities_si,
    std::int64_t source_timestamp_ns,
    std::int64_t received_timestamp_ns) noexcept {
  return publication_.publish(numeric_source_kind(source), source_joint_names,
                              positions_si, velocities_si,
                              source_timestamp_ns, received_timestamp_ns);
}

ObservedStateSnapshotDTO AddressedObservedStatePublication::make_snapshot(
    const ObservedRobotStatePublication::ReadView& view) const {
  const auto& context = config_.query_context;
  std::vector<CoordinatePositionDTO> positions;
  positions.reserve(context.coordinate_kinds.size());
  for (std::size_t index = 0U; index < context.coordinate_kinds.size(); ++index) {
    const double value = view.positions_rad()[index];
    if (context.coordinate_kinds[index] == CoordinateKind::kAngular) {
      positions.emplace_back(AngularPositionDTO{value});
    } else {
      positions.emplace_back(LinearPositionDTO{value});
    }
  }
  std::optional<std::vector<CoordinateVelocityDTO>> velocities;
  if (view.velocities_complete()) {
    velocities.emplace();
    velocities->reserve(context.coordinate_kinds.size());
    for (std::size_t index = 0U; index < context.coordinate_kinds.size();
         ++index) {
      const double value = view.velocities_rad_s()[index];
      if (context.coordinate_kinds[index] == CoordinateKind::kAngular) {
        velocities->emplace_back(AngularVelocityDTO{value});
      } else {
        velocities->emplace_back(LinearVelocityDTO{value});
      }
    }
  }
  return ObservedStateSnapshotDTO{
      context.controllable_component_id,
      context.command_target_id,
      context.control_domain_id,
      context.control_domain_revision,
      context.control_binding_id,
      context.control_binding_revision,
      context.provider_instance_id,
      context.provider_manifest_id,
      context.provider_manifest_revision,
      context.controller_topology_id,
      context.topology_revision,
      context.control_route_id,
      context.active_control_configuration_id,
      context.active_control_configuration_generation,
      context.resources,
      view.sequence(),
      context.joint_names,
      std::move(positions),
      std::move(velocities),
      std::string(view.source_id()),
      context.runtime_epoch,
      addressed_source_kind(view.source_kind()),
      view.source_timestamp_ns(),
      view.received_timestamp_ns(),
      context.state_authority_id,
      context.state_authority_class,
      config_.authority_generation,
      context.freshness_budget_ns,
      view.complete()};
}

Result<ObservedStateSnapshotDTO>
AddressedObservedStatePublication::current_state(
    const ObservedStateQueryContextDTO& context,
    std::int64_t now_ns) const {
  if (context != config_.query_context) {
    return Result<ObservedStateSnapshotDTO>::Err(
        state_error("observed_state_query_context_mismatch"));
  }
  auto view = publication_.acquire(now_ns);
  if (!view) {
    return Result<ObservedStateSnapshotDTO>::Err(
        state_error("observed_state_not_published"));
  }
  auto snapshot = make_snapshot(view);
  if (auto valid = validate_observed_state_snapshot(snapshot, now_ns);
      !valid.ok()) {
    return Result<ObservedStateSnapshotDTO>::Err(valid.error());
  }
  return Result<ObservedStateSnapshotDTO>::Ok(std::move(snapshot));
}

Result<ObservedStatePublicationDTO>
AddressedObservedStatePublication::publication(
    const ControlBindingId& control_binding_id,
    const ControlBindingRevision& control_binding_revision) const {
  const auto& context = config_.query_context;
  if (control_binding_id != context.control_binding_id ||
      control_binding_revision != context.control_binding_revision) {
    return Result<ObservedStatePublicationDTO>::Err(
        state_error("observed_state_publication_binding_mismatch"));
  }
  auto view = publication_.acquire_latest();
  if (!view) {
    return Result<ObservedStatePublicationDTO>::Err(
        state_error("observed_state_not_published"));
  }
  auto snapshot = make_snapshot(view);
  return Result<ObservedStatePublicationDTO>::Ok(ObservedStatePublicationDTO{
      context.controllable_component_id,
      context.command_target_id,
      context.control_domain_id,
      context.control_domain_revision,
      context.control_binding_id,
      context.control_binding_revision,
      context.controller_topology_id,
      context.topology_revision,
      context.control_route_id,
      context.active_control_configuration_id,
      context.active_control_configuration_generation,
      context.provider_instance_id,
      context.provider_manifest_id,
      context.provider_manifest_revision,
      context.state_authority_id,
      context.state_authority_class,
      context.runtime_epoch,
      config_.authority_generation,
      view.sequence(),
      std::move(snapshot),
      view.received_timestamp_ns()});
}

AddressedAngularObservedRobotStateAdapter::
    AddressedAngularObservedRobotStateAdapter(
        const AddressedObservedStatePublication& publication)
    : publication_(&publication) {
  const auto& kinds = publication.query_context().coordinate_kinds;
  if (kinds.empty() ||
      !std::all_of(kinds.begin(), kinds.end(), [](CoordinateKind kind) {
        return kind == CoordinateKind::kAngular;
      })) {
    throw std::invalid_argument(
        "legacy observed-state projection requires an all-angular target");
  }
}

Result<RobotStateDTO>
AddressedAngularObservedRobotStateAdapter::current_state(
    std::int64_t now_ns) const {
  auto result = publication_->current_state(publication_->query_context(),
                                            now_ns);
  if (!result.ok()) {
    return Result<RobotStateDTO>::Err(result.error());
  }
  RobotStateDTO state;
  state.joint_names = result.value().joint_names;
  state.positions_rad.reserve(result.value().positions.size());
  for (const auto& position : result.value().positions) {
    const auto* angular = std::get_if<AngularPositionDTO>(&position);
    if (angular == nullptr) {
      return Result<RobotStateDTO>::Err(
          state_error("legacy_angular_projection_unit_mismatch"));
    }
    state.positions_rad.push_back(angular->value_rad);
  }
  if (result.value().velocities.has_value()) {
    state.velocities_rad_s.reserve(result.value().velocities->size());
    for (const auto& velocity : *result.value().velocities) {
      const auto* angular = std::get_if<AngularVelocityDTO>(&velocity);
      if (angular == nullptr) {
        return Result<RobotStateDTO>::Err(
            state_error("legacy_angular_projection_unit_mismatch"));
      }
      state.velocities_rad_s.push_back(angular->value_rad_s);
    }
  }
  return Result<RobotStateDTO>::Ok(std::move(state));
}

const ObservedRobotStatePublication*
AddressedAngularObservedRobotStateAdapter::publication() const noexcept {
  return &publication_->numeric_publication();
}

}  // namespace manyforge::core
