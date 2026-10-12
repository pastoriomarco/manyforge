#include <manyforge_roboplan/retained_observed_state_channel.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
/// Control-facing capabilities on one command target may have distinct graph
/// binding/capability identities while still terminating at the same exact
/// backend occurrence. Keep that relationship separate from Program aliases,
/// which must preserve the complete control path.
[[nodiscard]] bool retained_bindings_share_backend_owner(
    const core::RuntimeParticipantBinding& primary,
    const core::RuntimeParticipantBinding& representative) noexcept {
  const auto& left = primary.value();
  const auto& right = representative.value();
  if (!left.control_path.has_value() || !right.control_path.has_value()) {
    return false;
  }
  const auto& left_path = *left.control_path;
  const auto& right_path = *right.control_path;
  return left.retained_bundle_id == right.retained_bundle_id &&
         left.graph_revision == right.graph_revision &&
         left.controllable_component_id == right.controllable_component_id &&
         left.component_type_id == right.component_type_id &&
         left.component_type_revision == right.component_type_revision &&
         left.command_target_id == right.command_target_id &&
         left.model_bindings == right.model_bindings && left.model_groups == right.model_groups &&
         left.frame_bindings == right.frame_bindings && left.resources == right.resources &&
         left.collision_body_bindings == right.collision_body_bindings &&
         left.runtime_unit_epochs == right.runtime_unit_epochs &&
         right.contact_grasp_bindings.empty() &&
         left_path.control_domain_id == right_path.control_domain_id &&
         left_path.control_domain_revision == right_path.control_domain_revision &&
         left_path.control_interface_contract_id == right_path.control_interface_contract_id &&
         left_path.control_interface_revision == right_path.control_interface_revision &&
         left_path.controller_topology_id == right_path.controller_topology_id &&
         left_path.topology_revision == right_path.topology_revision &&
         left_path.control_route_id == right_path.control_route_id &&
         left_path.activation_set_id == right_path.activation_set_id &&
         left_path.active_control_configuration_id == right_path.active_control_configuration_id &&
         left_path.active_control_configuration_generation ==
             right_path.active_control_configuration_generation &&
         left_path.provider_instance_id == right_path.provider_instance_id &&
         left_path.provider_manifest_id == right_path.provider_manifest_id &&
         left_path.provider_manifest_revision == right_path.provider_manifest_revision &&
         left_path.provider_runtime_unit_id == right_path.provider_runtime_unit_id &&
         left_path.provider_runtime_epoch == right_path.provider_runtime_epoch &&
         left_path.state_authority_id == right_path.state_authority_id &&
         left_path.state_authority_class == right_path.state_authority_class &&
         left_path.stop_evidence_policy_id == right_path.stop_evidence_policy_id &&
         left_path.stop_evidence_policy_revision == right_path.stop_evidence_policy_revision &&
         left_path.containment_granularity == right_path.containment_granularity;
}

[[nodiscard]] std::string_view
retained_product_reseal_physical_owner_difference(
    const core::RuntimeParticipantBinding &predecessor,
    const core::RuntimeParticipantBinding &candidate) {
  const auto &before = predecessor.value();
  auto after = candidate.value();
  // These fields describe the authored Program/product seal, not the physical
  // controller. The resolved model, frame, resource, route, provider and
  // runtime-unit facts remain byte-for-byte equal below. Contact/grasp
  // templates are consumed by admitted Program operations and do not alter
  // the retained controller itself.
  after.program_id = before.program_id;
  after.program_revision = before.program_revision;
  after.program_binding_id = before.program_binding_id;
  after.retained_bundle_id = before.retained_bundle_id;
  after.graph_revision = before.graph_revision;
  after.component_role_id = before.component_role_id;
  after.target_role_id = before.target_role_id;
  after.contact_grasp_bindings = before.contact_grasp_bindings;

  if (after.controllable_component_id != before.controllable_component_id)
    return "controllable_component_id";
  if (after.component_type_id != before.component_type_id) return "component_type_id";
  if (after.component_type_revision != before.component_type_revision)
    return "component_type_revision";
  if (after.command_target_id != before.command_target_id) return "command_target_id";
  if (after.capability_id != before.capability_id) return "capability_id";
  if (after.capability_definition_revision != before.capability_definition_revision)
    return "capability_definition_revision";
  if (after.command_schema != before.command_schema) return "command_schema";
  if (after.model_bindings != before.model_bindings) return "model_bindings";
  if (after.model_groups != before.model_groups) return "model_groups";
  if (after.frame_bindings != before.frame_bindings) return "frame_bindings";
  if (after.resources != before.resources) return "resources";
  if (after.collision_body_bindings != before.collision_body_bindings)
    return "collision_body_bindings";
  if (after.control_path.has_value() != before.control_path.has_value())
    return "control_path";
  if (after.control_path && before.control_path) {
    if (after.control_path->control_domain_id != before.control_path->control_domain_id)
      return "control_path.control_domain_id";
    if (after.control_path->control_domain_revision !=
        before.control_path->control_domain_revision)
      return "control_path.control_domain_revision";
    if (after.control_path->control_binding_id != before.control_path->control_binding_id)
      return "control_path.control_binding_id";
    if (after.control_path->control_binding_revision !=
        before.control_path->control_binding_revision)
      return "control_path.control_binding_revision";
    if (after.control_path->control_interface_contract_id !=
        before.control_path->control_interface_contract_id)
      return "control_path.control_interface_contract_id";
    if (after.control_path->control_interface_revision !=
        before.control_path->control_interface_revision)
      return "control_path.control_interface_revision";
    if (after.control_path->controller_topology_id !=
        before.control_path->controller_topology_id)
      return "control_path.controller_topology_id";
    if (after.control_path->topology_revision != before.control_path->topology_revision)
      return "control_path.topology_revision";
    if (after.control_path->control_route_id != before.control_path->control_route_id)
      return "control_path.control_route_id";
    if (after.control_path->activation_set_id != before.control_path->activation_set_id)
      return "control_path.activation_set_id";
    if (after.control_path->active_control_configuration_id !=
        before.control_path->active_control_configuration_id)
      return "control_path.active_control_configuration_id";
    if (after.control_path->active_control_configuration_generation !=
        before.control_path->active_control_configuration_generation)
      return "control_path.active_control_configuration_generation";
    if (after.control_path->provider_instance_id !=
        before.control_path->provider_instance_id)
      return "control_path.provider_instance_id";
    if (after.control_path->provider_manifest_id !=
        before.control_path->provider_manifest_id)
      return "control_path.provider_manifest_id";
    if (after.control_path->provider_manifest_revision !=
        before.control_path->provider_manifest_revision)
      return "control_path.provider_manifest_revision";
    if (after.control_path->provider_runtime_unit_id !=
        before.control_path->provider_runtime_unit_id)
      return "control_path.provider_runtime_unit_id";
    if (after.control_path->provider_runtime_epoch !=
        before.control_path->provider_runtime_epoch)
      return "control_path.provider_runtime_epoch";
    if (after.control_path->state_authority_id != before.control_path->state_authority_id)
      return "control_path.state_authority_id";
    if (after.control_path->state_authority_class !=
        before.control_path->state_authority_class)
      return "control_path.state_authority_class";
    if (after.control_path->stop_evidence_policy_id !=
        before.control_path->stop_evidence_policy_id)
      return "control_path.stop_evidence_policy_id";
    if (after.control_path->stop_evidence_policy_revision !=
        before.control_path->stop_evidence_policy_revision)
      return "control_path.stop_evidence_policy_revision";
    if (after.control_path->containment_granularity !=
        before.control_path->containment_granularity)
      return "control_path.containment_granularity";
    if (after.control_path->runtime_scale_qualification !=
        before.control_path->runtime_scale_qualification)
      return "control_path.runtime_scale_qualification";
  }
  if (after.capability_provider_path != before.capability_provider_path)
    return "capability_provider_path";
  if (after.supporting_capability_provider_paths !=
      before.supporting_capability_provider_paths)
    return "supporting_capability_provider_paths";
  if (after.runtime_unit_epochs != before.runtime_unit_epochs)
    return "runtime_unit_epochs";
  // Keep the exact DTO equality as the final check, including future fields.
  return after == before ? std::string_view{} : "binding";
}

[[nodiscard]] bool
retained_bindings_preserve_physical_owner_after_product_reseal(
    const core::RuntimeParticipantBinding &predecessor,
    const core::RuntimeParticipantBinding &candidate) {
  return retained_product_reseal_physical_owner_difference(predecessor, candidate)
      .empty();
}

namespace {

[[nodiscard]] core::ObservedRobotStateSourceKind
numeric_source_kind(const core::ObservedStateSourceKind source) noexcept {
  switch (source) {
  case core::ObservedStateSourceKind::kActionFeedback:
    return core::ObservedRobotStateSourceKind::kActionFeedback;
  case core::ObservedStateSourceKind::kControllerState:
    return core::ObservedRobotStateSourceKind::kControllerState;
  case core::ObservedStateSourceKind::kJointState:
    return core::ObservedRobotStateSourceKind::kJointState;
  case core::ObservedStateSourceKind::kCommandedState:
    return core::ObservedRobotStateSourceKind::kCommandedState;
  case core::ObservedStateSourceKind::kPredictedState:
    return core::ObservedRobotStateSourceKind::kPredictedState;
  }
  return core::ObservedRobotStateSourceKind::kJointState;
}

[[nodiscard]] core::ObservedStateSourceKind addressed_source_kind(
    const core::ObservedRobotStateSourceKind source) noexcept {
  switch (source) {
  case core::ObservedRobotStateSourceKind::kActionFeedback:
    return core::ObservedStateSourceKind::kActionFeedback;
  case core::ObservedRobotStateSourceKind::kControllerState:
    return core::ObservedStateSourceKind::kControllerState;
  case core::ObservedRobotStateSourceKind::kJointState:
    return core::ObservedStateSourceKind::kJointState;
  case core::ObservedRobotStateSourceKind::kCommandedState:
    return core::ObservedStateSourceKind::kCommandedState;
  case core::ObservedRobotStateSourceKind::kPredictedState:
    return core::ObservedStateSourceKind::kPredictedState;
  }
  return core::ObservedStateSourceKind::kJointState;
}

[[nodiscard]] core::ObservedRobotStateAuthorityProfile
authority_profile(const core::StateAuthorityClass authority) {
  switch (authority) {
  case core::StateAuthorityClass::kMeasured:
    return core::ObservedRobotStateAuthorityProfile::kMeasured;
  case core::StateAuthorityClass::kCommanded:
    return core::ObservedRobotStateAuthorityProfile::kCommandedSimulation;
  case core::StateAuthorityClass::kPredicted:
    return core::ObservedRobotStateAuthorityProfile::kPredictedNative;
  }
  throw std::invalid_argument("unknown retained state authority class");
}

[[nodiscard]] core::AddressedObservedStatePublicationConfig
publication_config(const core::RuntimeParticipantBinding &binding,
                   const RetainedObservedStateChannelConfig &config) {
  auto context = core::make_observed_state_query_context(
      binding, config.freshness_budget_ns);
  if (!context.ok()) {
    throw std::invalid_argument(context.error().message);
  }
  if (config.source_id.empty() || config.authority_generation == 0U) {
    throw std::invalid_argument(
        "retained observed-state channel has incomplete source identity");
  }
  const auto profile = authority_profile(context.value().state_authority_class);
  auto sources = config.source_precedence;
  if (sources.empty()) {
    sources.push_back(core::ObservedRobotStateSourceConfig{
        numeric_source_kind(config.source_kind), config.source_id, profile,
        config.freshness_budget_ns, "retained_native_state_authority"});
  } else if (sources.front().kind != numeric_source_kind(config.source_kind) ||
             sources.front().source_id != config.source_id ||
             sources.front().authority_profile != profile) {
    throw std::invalid_argument(
        "retained observed-state primary source differs from its addressed "
        "authority");
  }
  return core::AddressedObservedStatePublicationConfig{
      std::move(context.value()), std::move(sources),
      config.authority_generation, 256U};
}

[[nodiscard]] core::ErrorDTO channel_error(std::string reason,
                                           std::string message) {
  return core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
      std::move(message),
      {{"reason", std::move(reason)}}};
}

[[nodiscard]] bool
publication_backpressured(const std::string_view reason) noexcept {
  return reason == "source_writer_busy" ||
         reason == "publication_slots_exhausted";
}

[[nodiscard]] double
position_value(const core::CoordinatePositionDTO &position) noexcept {
  if (const auto *angular = std::get_if<core::AngularPositionDTO>(&position)) {
    return angular->value_rad;
  }
  return std::get<core::LinearPositionDTO>(position).value_m;
}

} // namespace

RetainedObservationWake::RetainedObservationWake()
    : incarnation_([] {
        // Presentation stream identity only; this counter owns no execution
        // state and retains no prior incarnation. Construction is NRT.
        static std::atomic<std::uint64_t> next{1U};
        auto value = next.load(std::memory_order_relaxed);
        do {
          if (value == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("observation incarnation exhausted");
        } while (!next.compare_exchange_weak(value, value + 1U, std::memory_order_relaxed));
        return value;
      }()) {}

void RetainedObservationWake::notify() noexcept {
  generation_.fetch_add(1U, std::memory_order_release);
  if (!pending_.exchange(true, std::memory_order_acq_rel)) wake_.release();
}

bool RetainedObservationWake::wait_after(
    const std::uint64_t after, const std::chrono::nanoseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (generation() == after) {
    if (!wake_.try_acquire_until(deadline)) return generation() != after;
    // A publication racing with this clear is visible in the next generation
    // read. Once cleared, the next publisher can release the single token.
    static_cast<void>(pending_.exchange(false, std::memory_order_acq_rel));
  }
  return true;
}

RetainedObservedStateChannel::RetainedObservedStateChannel(
    core::RuntimeParticipantBinding binding,
    std::shared_ptr<RetainedKinematicStateProjector> projector,
    RetainedObservedStateChannelConfig config, core::IClock *clock,
    std::shared_ptr<RetainedObservationWake> render_wake)
    : current_binding_(std::move(binding)),
      config_(std::move(config)), clock_(clock),
      publication_(publication_config(current_binding_, config_)),
      projection_context_(publication_.query_context()),
      render_wake_(render_wake ? std::move(render_wake)
                               : std::make_shared<RetainedObservationWake>()),
      last_render_positions_(projection_context_.resources.size()) {
  if (!projector) {
    if (!current_binding_.value().model_groups.empty() ||
        std::any_of(current_binding_.value().resources.begin(),
                    current_binding_.value().resources.end(),
                    [](const auto &resource) {
                      return resource.source_joint_name.has_value();
                    })) {
      throw std::invalid_argument(
          "component-local observed-state channel crosses a retained model");
    }
    return;
  }
  if (std::any_of(current_binding_.value().resources.begin(),
                  current_binding_.value().resources.end(),
                  [](const auto &resource) {
                    return !resource.source_joint_name.has_value();
                  })) {
    throw std::invalid_argument(
        "projected observed-state channel contains a component-local resource");
  }
  const auto &context = publication_.query_context();
  const auto contexts = projector->query_contexts();
  if (std::count(contexts.begin(), contexts.end(), context) != 1) {
    throw std::invalid_argument("retained observed-state channel target is not "
                                "registered exactly once");
  }
  projection_ = std::make_shared<const KinematicProjection>(
      KinematicProjection{std::move(projector), context});
}

core::Result<void> RetainedObservedStateChannel::ReplaceKinematicProjector(
    std::shared_ptr<RetainedKinematicStateProjector> candidate,
    const std::int64_t now_ns) {
  if (candidate == nullptr) {
    return core::Result<void>::Err(
        channel_error("retained_state_projector_missing",
                      "candidate kinematic state projector is absent"));
  }
  const auto context = publication_.query_context();
  const auto contexts = candidate->query_contexts();
  if (std::count(contexts.begin(), contexts.end(), context) != 1) {
    return core::Result<void>::Err(channel_error(
        "retained_state_projector_target_mismatch",
        "candidate model does not register this source's current target"));
  }
  {
    std::unique_lock lock(projection_mutex_);
    if (projection_ == nullptr) {
      return core::Result<void>::Err(channel_error(
          "component_local_state_has_no_model_projection",
          "component-local state has no model projection to replace"));
    }
    projection_ = std::make_shared<const KinematicProjection>(
        KinematicProjection{std::move(candidate), context});
  }
  auto projected =
      project_latest(now_ns, RetainedStateProjectionMode::kBlocking);
  if (!projected.applied()) {
    return core::Result<void>::Err(
        projected.error.value_or(channel_error(
            "retained_state_projection_not_applied",
            "latest owner state did not project into the candidate model")));
  }
  return core::Result<void>::Ok();
}

std::shared_ptr<const RetainedKinematicStateProjector>
RetainedObservedStateChannel::kinematic_projector() const {
  std::shared_lock lock(projection_mutex_);
  return projection_ != nullptr ? projection_->projector : nullptr;
}

core::Result<void> RetainedObservedStateChannel::RebindRoute(
    core::RuntimeParticipantBinding binding,
    const std::int64_t transition_time_ns) {
  try {
    auto context = core::make_observed_state_query_context(
        binding, config_.freshness_budget_ns);
    if (!context.ok()) {
      return core::Result<void>::Err(context.error());
    }
    std::lock_guard lock(route_rebind_mutex_);
    auto rebound_context = std::move(context.value());
    if (rebound_context == publication_.query_context()) {
      // A route readback may re-present the already-installed identity.
      // Delegate validation to the addressed owner without opening a
      // freshness gate for this no-op.
      auto rebound = publication_.rebind_route_context(
          std::move(rebound_context), transition_time_ns);
      if (!rebound.ok()) {
        return rebound;
      }
      if (binding.value() != current_binding_.value()) {
        current_binding_ = std::move(binding);
        notify_render_change();
      }
      return core::Result<void>::Ok();
    }
    const bool was_fresh_state_required =
        fresh_state_required_.load(std::memory_order_acquire);
    fresh_state_required_.store(true, std::memory_order_release);
    auto rebound = publication_.rebind_route_context(std::move(rebound_context),
                                                     transition_time_ns);
    if (!rebound.ok()) {
      fresh_state_required_.store(was_fresh_state_required,
                                  std::memory_order_release);
      fresh_state_cv_.notify_all();
      return rebound;
    }
    current_binding_ = std::move(binding);
    // AddressedObservedStatePublication has just republished the complete
    // owner sample at the transition time. It is therefore current under the
    // new route and must not be stranded behind the recovery freshness gate.
    fresh_state_required_.store(false, std::memory_order_release);
    fresh_sample_generation_.fetch_add(1U, std::memory_order_acq_rel);
    notify_render_change();
    fresh_state_cv_.notify_all();
    return core::Result<void>::Ok();
  } catch (const std::exception &) {
    return core::Result<void>::Err(
        channel_error("retained_state_route_rebind_failed",
                      "native route-state binding could not be retained"));
  } catch (...) {
    return core::Result<void>::Err(
        channel_error("retained_state_route_rebind_failed",
                      "native route-state binding could not be retained"));
  }
}

RetainedStateProjectionResult
RetainedObservedStateChannel::initialize_from_projection(
    const std::int64_t timestamp_ns) {
  const auto projector = kinematic_projector();
  if (!projector) {
    return {
        RetainedStateProjectionStatus::kRejected,
        channel_error("component_local_state_has_no_model_projection",
                      "component-local state cannot be seeded from a model")};
  }
  auto state = projector->current_state(
      publication_.query_context().control_binding_id,
      publication_.query_context().control_binding_revision);
  if (!state.ok()) {
    return {RetainedStateProjectionStatus::kFailed, state.error()};
  }
  std::vector<double> positions;
  positions.reserve(state.value().positions.size());
  for (const auto &position : state.value().positions) {
    positions.push_back(position_value(position));
  }
  std::vector<double> velocities(positions.size(), 0.0);
  return publish_and_project(positions, velocities, timestamp_ns, timestamp_ns,
                             RetainedStateProjectionMode::kBlocking);
}

RetainedStateProjectionResult RetainedObservedStateChannel::publish_and_project(
    const std::span<const double> positions_si,
    const std::span<const double> velocities_si,
    const std::int64_t source_timestamp_ns,
    const std::int64_t received_timestamp_ns,
    const RetainedStateProjectionMode mode) {
  auto published = publish_state(positions_si, velocities_si,
                                 source_timestamp_ns, received_timestamp_ns);
  if (!published.applied()) {
    return published;
  }
  return project_latest(received_timestamp_ns, mode);
}

RetainedStateProjectionResult RetainedObservedStateChannel::publish_state(
    const std::span<const double> positions_si,
    const std::span<const double> velocities_si,
    const std::int64_t source_timestamp_ns,
    const std::int64_t received_timestamp_ns) {
  std::lock_guard route_lock(route_rebind_mutex_);
  // Route changes cannot alter physical shape/names; reuse the construction
  // context on this per-sample path instead of copying routing metadata.
  const auto &context = projection_context_;
  if (positions_si.size() != context.resources.size() ||
      (!velocities_si.empty() &&
       velocities_si.size() != context.resources.size())) {
    return {RetainedStateProjectionStatus::kRejected,
            channel_error("retained_state_channel_shape_mismatch",
                          "native state does not match the retained target")};
  }

  const auto published = publication_.publish(
      config_.source_kind, context.joint_names, positions_si, velocities_si,
      source_timestamp_ns, received_timestamp_ns);
  if (!published.published || !published.complete) {
    if (published.published) record_render_unavailable();
    const std::string reason = published.reason == nullptr
                                   ? "retained_state_publication_failed"
                                   : published.reason;
    return {
        publication_backpressured(reason)
            ? RetainedStateProjectionStatus::kBusy
            : RetainedStateProjectionStatus::kRejected,
        channel_error(reason, "native state publication was not committed")};
  }
  fresh_state_required_.store(false, std::memory_order_release);
  fresh_sample_generation_.fetch_add(1U, std::memory_order_acq_rel);
  record_render_positions(context.joint_names, positions_si);
  fresh_state_cv_.notify_all();
  return {RetainedStateProjectionStatus::kApplied, std::nullopt};
}

core::ObservedRobotStatePublishResult
RetainedObservedStateChannel::publish_adapter_state(
    const core::ObservedRobotStateSourceKind source,
    const std::span<const std::string> source_joint_names,
    const std::span<const double> positions_si,
    const std::span<const double> velocities_si,
    const std::int64_t source_timestamp_ns,
    const std::int64_t received_timestamp_ns) {
  std::lock_guard route_lock(route_rebind_mutex_);
  const auto published = publication_.publish(
      addressed_source_kind(source), source_joint_names, positions_si,
      velocities_si, source_timestamp_ns, received_timestamp_ns);
  if (published.published) {
    if (published.complete) {
      record_render_positions(source_joint_names, positions_si);
      // Positions suffice for rendering, never for motion-rest recovery.
      if (published.velocities_complete) {
        fresh_state_required_.store(false, std::memory_order_release);
        fresh_sample_generation_.fetch_add(1U, std::memory_order_acq_rel);
        fresh_state_cv_.notify_all();
      }
    } else {
      record_render_unavailable();
    }
  }
  return published;
}

RetainedStateProjectionResult
RetainedObservedStateChannel::publish_state_bounded(
    const std::span<const double> positions_si,
    const std::span<const double> velocities_si,
    const std::int64_t source_timestamp_ns,
    const std::int64_t received_timestamp_ns, const std::int64_t deadline_ns,
    const std::stop_token cancellation) {
  const auto is_expired = [this, deadline_ns,
                           received_timestamp_ns]() noexcept {
    if (deadline_ns < 0) {
      return false;
    }
    const auto now = clock_ == nullptr ? received_timestamp_ns : clock_->now();
    return now >= deadline_ns || received_timestamp_ns >= deadline_ns;
  };
  if (cancellation.stop_requested() || is_expired()) {
    return {
        RetainedStateProjectionStatus::kFailed,
        channel_error(cancellation.stop_requested()
                          ? "retained_state_publication_cancelled"
                          : "retained_state_publication_deadline_expired",
                      cancellation.stop_requested()
                          ? "native state publication was cancelled"
                          : "native state publication missed its deadline")};
  }
  std::unique_lock route_lock(route_rebind_mutex_, std::try_to_lock);
  if (!route_lock.owns_lock()) {
    return {RetainedStateProjectionStatus::kFailed,
            channel_error("retained_state_route_busy",
                          "native state publication route is busy")};
  }
  if (cancellation.stop_requested() || is_expired()) {
    return {
        RetainedStateProjectionStatus::kFailed,
        channel_error(cancellation.stop_requested()
                          ? "retained_state_publication_cancelled"
                          : "retained_state_publication_deadline_expired",
                      cancellation.stop_requested()
                          ? "native state publication was cancelled"
                          : "native state publication missed its deadline")};
  }
  const auto &context = projection_context_;
  if (positions_si.size() != context.resources.size() ||
      (!velocities_si.empty() &&
       velocities_si.size() != context.resources.size())) {
    return {RetainedStateProjectionStatus::kRejected,
            channel_error("retained_state_channel_shape_mismatch",
                          "native state does not match the retained target")};
  }
  const auto published = publication_.publish(
      config_.source_kind, context.joint_names, positions_si, velocities_si,
      source_timestamp_ns, received_timestamp_ns);
  if (cancellation.stop_requested() || is_expired()) {
    return {
        RetainedStateProjectionStatus::kFailed,
        channel_error(cancellation.stop_requested()
                          ? "retained_state_publication_cancelled"
                          : "retained_state_publication_deadline_expired",
                      cancellation.stop_requested()
                          ? "native state publication was cancelled"
                          : "native state publication missed its deadline")};
  }
  if (!published.published || !published.complete ||
      !published.velocities_complete) {
    if (published.published) {
      if (published.complete) record_render_positions(context.joint_names, positions_si);
      else record_render_unavailable();
    }
    const std::string reason = published.reason == nullptr
                                   ? "retained_state_publication_failed"
                                   : published.reason;
    return {
        publication_backpressured(reason)
            ? RetainedStateProjectionStatus::kBusy
            : RetainedStateProjectionStatus::kFailed,
        channel_error(reason, "native state publication was not committed")};
  }
  fresh_state_required_.store(false, std::memory_order_release);
  fresh_sample_generation_.fetch_add(1U, std::memory_order_acq_rel);
  record_render_positions(context.joint_names, positions_si);
  fresh_state_cv_.notify_all();
  if (cancellation.stop_requested() || is_expired()) {
    return {
        RetainedStateProjectionStatus::kFailed,
        channel_error(cancellation.stop_requested()
                          ? "retained_state_publication_cancelled"
                          : "retained_state_publication_deadline_expired",
                      cancellation.stop_requested()
                          ? "native state publication was cancelled"
                          : "native state publication missed its deadline")};
  }
  return {RetainedStateProjectionStatus::kApplied, std::nullopt};
}

RetainedStateProjectionResult RetainedObservedStateChannel::project_latest(
    const std::int64_t now_ns, const RetainedStateProjectionMode mode) {
  const auto &context = publication_.query_context();
  auto snapshot = publication_.current_state(context, now_ns);
  if (!snapshot.ok()) {
    return {RetainedStateProjectionStatus::kFailed, snapshot.error()};
  }
  std::shared_lock projection_lock(projection_mutex_);
  if (!projection_) {
    return {RetainedStateProjectionStatus::kApplied, std::nullopt};
  }
  // The scene projection is keyed by the route identity the selected
  // projector registered; owner route publication never re-keys it.
  const auto &registered = projection_->registered_context;
  auto projected = std::move(snapshot.value());
  projected.control_route_id = registered.control_route_id;
  projected.active_control_configuration_id =
      registered.active_control_configuration_id;
  projected.active_control_configuration_generation =
      registered.active_control_configuration_generation;
  return projection_->projector->apply(projected, now_ns, mode);
}

core::Result<core::ObservedStateSnapshotDTO>
RetainedObservedStateChannel::current_state(const std::int64_t now_ns) const {
  std::lock_guard route_lock(route_rebind_mutex_);
  if (fresh_state_required_.load(std::memory_order_acquire)) {
    return core::Result<core::ObservedStateSnapshotDTO>::Err(channel_error(
        "retained_state_fresh_sample_unavailable",
        "native route state requires a fresh current-binding sample"));
  }
  return publication_.current_state(publication_.query_context(), now_ns);
}

core::Result<RetainedBoundObservedState>
RetainedObservedStateChannel::current_bound_state(
    const std::int64_t now_ns) const {
  std::lock_guard route_lock(route_rebind_mutex_);
  if (fresh_state_required_.load(std::memory_order_acquire)) {
    return core::Result<RetainedBoundObservedState>::Err(channel_error(
        "retained_state_fresh_sample_unavailable",
        "native route state requires a fresh current-binding sample"));
  }
  auto state =
      publication_.current_state(publication_.query_context(), now_ns);
  if (!state.ok()) {
    return core::Result<RetainedBoundObservedState>::Err(state.error());
  }
  return core::Result<RetainedBoundObservedState>::Ok(
      RetainedBoundObservedState{current_binding_, std::move(state.value())});
}

core::Result<core::ObservedStateSnapshotDTO>
RetainedObservedStateChannel::current_state_bounded(
    const std::int64_t now_ns, const std::int64_t deadline_ns,
    const std::stop_token cancellation) const {
  const auto is_expired = [this, deadline_ns, now_ns]() noexcept {
    if (deadline_ns < 0) {
      return false;
    }
    const auto now = clock_ == nullptr ? now_ns : clock_->now();
    return now >= deadline_ns;
  };
  if (cancellation.stop_requested() || is_expired()) {
    return core::Result<core::ObservedStateSnapshotDTO>::Err(channel_error(
        cancellation.stop_requested() ? "retained_state_read_cancelled"
                                      : "retained_state_read_deadline_expired",
        cancellation.stop_requested()
            ? "native state read was cancelled"
            : "native state read missed its deadline"));
  }
  std::unique_lock route_lock(route_rebind_mutex_, std::try_to_lock);
  if (!route_lock.owns_lock()) {
    return core::Result<core::ObservedStateSnapshotDTO>::Err(channel_error(
        "retained_state_route_busy", "native state route is busy"));
  }
  if (cancellation.stop_requested() || is_expired()) {
    return core::Result<core::ObservedStateSnapshotDTO>::Err(channel_error(
        cancellation.stop_requested() ? "retained_state_read_cancelled"
                                      : "retained_state_read_deadline_expired",
        cancellation.stop_requested()
            ? "native state read was cancelled"
            : "native state read missed its deadline"));
  }
  if (fresh_state_required_.load(std::memory_order_acquire)) {
    return core::Result<core::ObservedStateSnapshotDTO>::Err(channel_error(
        "retained_state_fresh_sample_unavailable",
        "native route state requires a fresh current-binding sample"));
  }
  // The addressed owner selects context and sample together, with a try-lock.
  // No extra blocking context copy may enter this deadline-bounded path.
  auto result = publication_.publication_at(
      projection_context_.control_binding_id,
      projection_context_.control_binding_revision, now_ns);
  if (cancellation.stop_requested() || is_expired()) {
    return core::Result<core::ObservedStateSnapshotDTO>::Err(channel_error(
        cancellation.stop_requested() ? "retained_state_read_cancelled"
                                      : "retained_state_read_deadline_expired",
        cancellation.stop_requested()
            ? "native state read was cancelled"
            : "native state read missed its deadline"));
  }
  if (!result.ok())
    return core::Result<core::ObservedStateSnapshotDTO>::Err(result.error());
  return core::Result<core::ObservedStateSnapshotDTO>::Ok(
      std::move(result.value().snapshot));
}

core::Result<core::ControlledStateDTO>
RetainedObservedStateChannel::current_controlled_state() const {
  std::lock_guard route_lock(route_rebind_mutex_);
  if (fresh_state_required_.load(std::memory_order_acquire)) {
    return core::Result<core::ControlledStateDTO>::Err(channel_error(
        "retained_state_fresh_sample_unavailable",
        "native route state requires a fresh current-binding sample"));
  }
  const auto &context = publication_.query_context();
  auto current = publication_.publication(context.control_binding_id,
                                          context.control_binding_revision);
  if (!current.ok()) {
    return core::Result<core::ControlledStateDTO>::Err(current.error());
  }
  const auto &snapshot = current.value().snapshot;
  core::ControlledStateDTO state{
      snapshot.controllable_component_id,
      snapshot.command_target_id,
      snapshot.control_domain_id,
      snapshot.control_domain_revision,
      snapshot.control_binding_id,
      snapshot.control_binding_revision,
      snapshot.controller_topology_id,
      snapshot.topology_revision,
      snapshot.control_route_id,
      snapshot.active_control_configuration_id,
      snapshot.active_control_configuration_generation,
      snapshot.resources,
      snapshot.joint_names,
      snapshot.positions,
      snapshot.velocities};
  auto valid = core::validate_controlled_state(state);
  if (!valid.ok()) {
    return core::Result<core::ControlledStateDTO>::Err(valid.error());
  }
  return core::Result<core::ControlledStateDTO>::Ok(std::move(state));
}

bool RetainedObservedStateChannel::wait_for_fresh_state(
    const std::chrono::nanoseconds timeout) const {
  std::unique_lock lock(fresh_state_mutex_);
  if (!fresh_state_required_.load(std::memory_order_acquire)) {
    return true;
  }
  return fresh_state_cv_.wait_for(lock, timeout, [this] {
    return !fresh_state_required_.load(std::memory_order_acquire);
  });
}

bool RetainedObservedStateChannel::wait_for_render_change(
    const std::uint64_t after_generation,
    const std::chrono::nanoseconds timeout) const {
  return render_wake_->wait_after(after_generation, timeout);
}

void RetainedObservedStateChannel::record_render_positions(
    const std::span<const std::string> source_joint_names,
    const std::span<const double> positions_si) {
  const auto &canonical_names = projection_context_.joint_names;
  if (source_joint_names.size() != positions_si.size() ||
      canonical_names.size() != last_render_positions_.size()) {
    return;
  }
  bool changed = !render_positions_valid_;
  for (std::size_t canonical_index = 0U;
       canonical_index < canonical_names.size(); ++canonical_index) {
    const auto source =
        std::find(source_joint_names.begin(), source_joint_names.end(),
                  canonical_names[canonical_index]);
    if (source == source_joint_names.end()) {
      return;
    }
    const auto source_index = static_cast<std::size_t>(
        std::distance(source_joint_names.begin(), source));
    changed = changed || last_render_positions_[canonical_index] !=
                             positions_si[source_index];
  }
  for (std::size_t canonical_index = 0U;
       canonical_index < canonical_names.size(); ++canonical_index) {
    const auto source =
        std::find(source_joint_names.begin(), source_joint_names.end(),
                  canonical_names[canonical_index]);
    const auto source_index = static_cast<std::size_t>(
        std::distance(source_joint_names.begin(), source));
    last_render_positions_[canonical_index] = positions_si[source_index];
  }
  render_positions_valid_ = true;
  if (changed) {
    notify_render_change();
  }
}

void RetainedObservedStateChannel::notify_render_change() noexcept {
  render_wake_->notify();
}

void RetainedObservedStateChannel::record_render_unavailable() noexcept {
  if (render_positions_valid_) {
    render_positions_valid_ = false;
    notify_render_change();
  }
}

} // namespace manyforge::roboplan_backend
