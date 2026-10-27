#include <algorithm>
#include <manyforge_core/addressed_observed_state_publication.hpp>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] ErrorDTO state_error(std::string reason) {
  return ErrorDTO{"invalid_request",
                  "addressed observed state is unavailable",
                  {{"reason", std::move(reason)}}};
}

[[nodiscard]] ObservedRobotStateAuthorityProfile authority_profile(StateAuthorityClass authority) {
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

[[nodiscard]] bool source_kind_matches_authority(ObservedRobotStateSourceKind source,
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

[[nodiscard]] bool same_physical_query_context(const ObservedStateQueryContextDTO& left,
                                               const ObservedStateQueryContextDTO& right) noexcept {
  return left.controllable_component_id == right.controllable_component_id &&
         left.command_target_id == right.command_target_id &&
         left.capability_id == right.capability_id &&
         left.control_domain_id == right.control_domain_id &&
         left.control_domain_revision == right.control_domain_revision &&
         left.control_binding_id == right.control_binding_id &&
         left.control_binding_revision == right.control_binding_revision &&
         left.controller_topology_id == right.controller_topology_id &&
         left.topology_revision == right.topology_revision && left.resources == right.resources &&
         left.joint_names == right.joint_names && left.coordinate_kinds == right.coordinate_kinds &&
         left.control_interface_contract_id == right.control_interface_contract_id &&
         left.control_interface_revision == right.control_interface_revision &&
         left.provider_instance_id == right.provider_instance_id &&
         left.provider_manifest_id == right.provider_manifest_id &&
         left.provider_manifest_revision == right.provider_manifest_revision &&
         left.runtime_epoch == right.runtime_epoch &&
         left.state_authority_id == right.state_authority_id &&
         left.state_authority_class == right.state_authority_class &&
         left.freshness_budget_ns == right.freshness_budget_ns;
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
  if (context.controllable_component_id.empty() || context.command_target_id.empty() ||
      context.capability_id.empty() || context.control_domain_id.empty() ||
      context.control_domain_revision.empty() || context.control_binding_id.empty() ||
      context.control_binding_revision.empty() || context.controller_topology_id.empty() ||
      context.topology_revision.empty() || context.control_route_id.empty() ||
      context.control_interface_contract_id.empty() || context.control_interface_revision.empty() ||
      context.active_control_configuration_id.empty() ||
      !context.active_control_configuration_generation.nonzero() ||
      context.provider_instance_id.empty() || context.provider_manifest_id.empty() ||
      context.provider_manifest_revision.empty() || context.runtime_epoch.empty() ||
      context.state_authority_id.empty() || config.authority_generation == 0U) {
    throw std::invalid_argument("addressed observed-state publication has incomplete identity");
  }
  if (context.resources.empty() || context.resources.size() != context.joint_names.size() ||
      context.resources.size() != context.coordinate_kinds.size() ||
      context.resources.size() > config.maximum_source_resource_count) {
    throw std::invalid_argument("addressed observed-state publication has invalid resource shape");
  }
  std::set<ResourceId> resources;
  std::set<std::string> names;
  for (std::size_t index = 0U; index < context.resources.size(); ++index) {
    if (context.resources[index].empty() || context.joint_names[index].empty() ||
        !resources.insert(context.resources[index]).second ||
        !names.insert(context.joint_names[index]).second) {
      throw std::invalid_argument("addressed observed-state resources must be nonempty and unique");
    }
  }
  if (context.freshness_budget_ns < 0 ||
      (context.state_authority_class == StateAuthorityClass::kMeasured &&
       context.freshness_budget_ns <= 0)) {
    throw std::invalid_argument("measured addressed state requires a positive freshness budget");
  }
  if (config.source_precedence.empty()) {
    throw std::invalid_argument("addressed observed-state publication requires a source");
  }
  const auto expected_profile = authority_profile(context.state_authority_class);
  for (const auto& source : config.source_precedence) {
    if (source.source_id.empty() || source.authority_profile != expected_profile ||
        source.freshness_budget_ns != context.freshness_budget_ns ||
        !source_kind_matches_authority(source.kind, context.state_authority_class)) {
      throw std::invalid_argument("observed-state source disagrees with the retained authority");
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
      publication_(config_.query_context.joint_names, config_.source_precedence,
                   config_.maximum_source_resource_count),
      current_query_context_(config_.query_context) {}

Result<void> AddressedObservedStatePublication::rebind_route_context(
    ObservedStateQueryContextDTO query_context, const std::int64_t transition_time_ns) {
  try {
    AddressedObservedStatePublicationConfig candidate_config{
        query_context, config_.source_precedence, config_.authority_generation,
        config_.maximum_source_resource_count};
    validate_config(candidate_config);
    if (transition_time_ns < 0) {
      return Result<void>::Err(state_error("observed_state_route_rebind_not_permitted"));
    }
    std::lock_guard lock(route_context_mutex_);
    if (query_context == current_query_context_) {
      return Result<void>::Ok();
    }
    if (!same_physical_query_context(current_query_context_, query_context)) {
      return Result<void>::Err(state_error("observed_state_route_rebind_identity_mismatch"));
    }
    if (query_context.state_authority_class == StateAuthorityClass::kMeasured) {
      // A measurement is never re-stamped: only samples published after this
      // fence are current under the new route/configuration identity.
      context_first_sequence_ = publication_.next_sequence();
      current_query_context_ = std::move(query_context);
      return Result<void>::Ok();
    }
    auto latest = publication_.acquire_latest();
    if (!latest || !latest.complete() || !latest.velocities_complete()) {
      return Result<void>::Err(state_error("observed_state_route_rebind_source_unavailable"));
    }
    if (!validate_observed_state_snapshot(make_snapshot(latest, current_query_context_),
                                          transition_time_ns)
             .ok()) {
      return Result<void>::Err(state_error("observed_state_route_rebind_source_not_current"));
    }

    const auto seeded =
        publication_.publish(latest.source_kind(), latest.joint_names(), latest.positions_rad(),
                             latest.velocities_rad_s(), transition_time_ns, transition_time_ns);
    if (!seeded.published || !seeded.complete || !seeded.velocities_complete) {
      return Result<void>::Err(state_error("observed_state_route_rebind_publish_failed"));
    }
    current_query_context_ = std::move(query_context);
    context_first_sequence_ = seeded.sequence;
    return Result<void>::Ok();
  } catch (const std::exception&) {
    return Result<void>::Err(state_error("observed_state_route_rebind_capacity_unavailable"));
  } catch (...) {
    return Result<void>::Err(state_error("observed_state_route_rebind_capacity_unavailable"));
  }
}

ObservedStateQueryContextDTO AddressedObservedStatePublication::query_context() const {
  std::shared_lock lock(route_context_mutex_);
  return current_query_context_;
}

ObservedRobotStatePublishResult AddressedObservedStatePublication::publish(
    ObservedStateSourceKind source, std::span<const std::string> source_joint_names,
    std::span<const double> positions_si, std::span<const double> velocities_si,
    std::int64_t source_timestamp_ns, std::int64_t received_timestamp_ns) noexcept {
  return publication_.publish(numeric_source_kind(source), source_joint_names, positions_si,
                              velocities_si, source_timestamp_ns, received_timestamp_ns);
}

ObservedStateSnapshotDTO AddressedObservedStatePublication::make_snapshot(
    const ObservedRobotStatePublication::ReadView& view,
    const ObservedStateQueryContextDTO& context) const {
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
    for (std::size_t index = 0U; index < context.coordinate_kinds.size(); ++index) {
      const double value = view.velocities_rad_s()[index];
      if (context.coordinate_kinds[index] == CoordinateKind::kAngular) {
        velocities->emplace_back(AngularVelocityDTO{value});
      } else {
        velocities->emplace_back(LinearVelocityDTO{value});
      }
    }
  }
  return ObservedStateSnapshotDTO{context.controllable_component_id,
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

ObservedStatePublicationDTO AddressedObservedStatePublication::make_publication(
    const ObservedRobotStatePublication::ReadView& view,
    const ObservedStateQueryContextDTO& context) const {
  auto snapshot = make_snapshot(view, context);
  return ObservedStatePublicationDTO{context.controllable_component_id,
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
                                     view.received_timestamp_ns()};
}

Result<ObservedStateSnapshotDTO> AddressedObservedStatePublication::current_state(
    const ObservedStateQueryContextDTO& context, std::int64_t now_ns) const {
  std::shared_lock lock(route_context_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return Result<ObservedStateSnapshotDTO>::Err(state_error("observed_state_route_context_busy"));
  }
  const auto& current = current_query_context_;
  if (context != current) {
    return Result<ObservedStateSnapshotDTO>::Err(
        state_error("observed_state_query_context_mismatch"));
  }
  // The caller's timestamp defines the observation boundary. A concurrent
  // publisher may commit a newer sample after that timestamp but before this
  // read; prefer the newest retained sample that actually existed as-of the
  // boundary. Fall back to the latest view only to preserve a precise stale or
  // future-sample validation error when no as-of sample exists.
  auto view = publication_.acquire_as_of(now_ns);
  if (!view) {
    view = publication_.acquire(now_ns);
  }
  if (!view) {
    return Result<ObservedStateSnapshotDTO>::Err(state_error("observed_state_not_published"));
  }
  if (view.sequence() < context_first_sequence_) {
    return Result<ObservedStateSnapshotDTO>::Err(
        state_error("observed_state_context_not_current_at_evaluation_time"));
  }
  auto snapshot = make_snapshot(view, current);
  if (auto valid = validate_observed_state_snapshot(snapshot, now_ns); !valid.ok()) {
    return Result<ObservedStateSnapshotDTO>::Err(valid.error());
  }
  return Result<ObservedStateSnapshotDTO>::Ok(std::move(snapshot));
}

Result<ObservedStatePublicationDTO> AddressedObservedStatePublication::publication(
    const ControlBindingId& control_binding_id,
    const ControlBindingRevision& control_binding_revision) const {
  std::shared_lock lock(route_context_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return Result<ObservedStatePublicationDTO>::Err(
        state_error("observed_state_route_context_busy"));
  }
  const auto& context = current_query_context_;
  if (control_binding_id != context.control_binding_id ||
      control_binding_revision != context.control_binding_revision) {
    return Result<ObservedStatePublicationDTO>::Err(
        state_error("observed_state_publication_binding_mismatch"));
  }
  auto view = publication_.acquire_latest();
  if (!view) {
    return Result<ObservedStatePublicationDTO>::Err(state_error("observed_state_not_published"));
  }
  if (view.sequence() < context_first_sequence_) {
    return Result<ObservedStatePublicationDTO>::Err(
        state_error("observed_state_context_not_current_at_evaluation_time"));
  }
  return Result<ObservedStatePublicationDTO>::Ok(make_publication(view, context));
}

Result<ObservedStatePublicationDTO> AddressedObservedStatePublication::publication_at(
    const ControlBindingId& control_binding_id,
    const ControlBindingRevision& control_binding_revision, std::int64_t now_ns) const {
  std::shared_lock lock(route_context_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return Result<ObservedStatePublicationDTO>::Err(
        state_error("observed_state_route_context_busy"));
  }
  const auto& context = current_query_context_;
  if (control_binding_id != context.control_binding_id ||
      control_binding_revision != context.control_binding_revision) {
    return Result<ObservedStatePublicationDTO>::Err(
        state_error("observed_state_publication_binding_mismatch"));
  }
  if (now_ns < 0) {
    return Result<ObservedStatePublicationDTO>::Err(
        state_error("observed_state_evaluation_time_invalid"));
  }
  auto view = publication_.acquire_as_of(now_ns);
  if (!view || view.sequence() < context_first_sequence_) {
    return Result<ObservedStatePublicationDTO>::Err(
        state_error("observed_state_unavailable_at_evaluation_time"));
  }
  auto result = make_publication(view, context);
  if (auto valid = validate_observed_state_publication(result, now_ns); !valid.ok()) {
    return Result<ObservedStatePublicationDTO>::Err(valid.error());
  }
  return Result<ObservedStatePublicationDTO>::Ok(std::move(result));
}

AddressedAngularObservedRobotStateAdapter::AddressedAngularObservedRobotStateAdapter(
    const AddressedObservedStatePublication& publication)
    : publication_(&publication) {
  const auto& kinds = publication.query_context().coordinate_kinds;
  if (kinds.empty() || !std::all_of(kinds.begin(), kinds.end(), [](CoordinateKind kind) {
        return kind == CoordinateKind::kAngular;
      })) {
    throw std::invalid_argument("legacy observed-state projection requires an all-angular target");
  }
}

Result<RobotStateDTO> AddressedAngularObservedRobotStateAdapter::current_state(
    std::int64_t now_ns) const {
  auto result = publication_->current_state(publication_->query_context(), now_ns);
  if (!result.ok()) {
    return Result<RobotStateDTO>::Err(result.error());
  }
  RobotStateDTO state;
  state.joint_names = result.value().joint_names;
  state.positions_rad.reserve(result.value().positions.size());
  for (const auto& position : result.value().positions) {
    const auto* angular = std::get_if<AngularPositionDTO>(&position);
    if (angular == nullptr) {
      return Result<RobotStateDTO>::Err(state_error("legacy_angular_projection_unit_mismatch"));
    }
    state.positions_rad.push_back(angular->value_rad);
  }
  if (result.value().velocities.has_value()) {
    state.velocities_rad_s.reserve(result.value().velocities->size());
    for (const auto& velocity : *result.value().velocities) {
      const auto* angular = std::get_if<AngularVelocityDTO>(&velocity);
      if (angular == nullptr) {
        return Result<RobotStateDTO>::Err(state_error("legacy_angular_projection_unit_mismatch"));
      }
      state.velocities_rad_s.push_back(angular->value_rad_s);
    }
  }
  return Result<RobotStateDTO>::Ok(std::move(state));
}

const ObservedRobotStatePublication* AddressedAngularObservedRobotStateAdapter::publication()
    const noexcept {
  return &publication_->numeric_publication();
}

Result<ObservedStatePublicationDTO>
AddressedAngularObservedRobotStateAdapter::addressed_publication(
    const ControlBindingId& control_binding_id,
    const ControlBindingRevision& control_binding_revision) const {
  return publication_->publication(control_binding_id, control_binding_revision);
}

}  // namespace manyforge::core
