#include <manyforge_roboplan/retained_observed_state_channel.hpp>

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
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

RetainedObservedStateChannel::RetainedObservedStateChannel(
    core::RuntimeParticipantBinding binding,
    std::shared_ptr<RetainedKinematicStateProjector> projector,
    RetainedObservedStateChannelConfig config, core::IClock *clock)
    : initial_binding_(std::move(binding)), projector_(std::move(projector)),
      config_(std::move(config)), clock_(clock),
      publication_(publication_config(initial_binding_, config_)),
      projection_context_(publication_.query_context()),
      last_render_positions_(publication_.query_context().resources.size()),
      current_binding_(&initial_binding_) {
  retained_route_bindings_.reserve(4096U);
  if (!projector_) {
    if (!initial_binding_.value().model_groups.empty() ||
        std::any_of(initial_binding_.value().resources.begin(),
                    initial_binding_.value().resources.end(),
                    [](const auto &resource) {
                      return resource.source_joint_name.has_value();
                    })) {
      throw std::invalid_argument(
          "component-local observed-state channel crosses a retained model");
    }
    return;
  }
  if (std::any_of(initial_binding_.value().resources.begin(),
                  initial_binding_.value().resources.end(),
                  [](const auto &resource) {
                    return !resource.source_joint_name.has_value();
                  })) {
    throw std::invalid_argument(
        "projected observed-state channel contains a component-local resource");
  }
  const auto &context = publication_.query_context();
  const auto contexts = projector_->query_contexts();
  if (std::count(contexts.begin(), contexts.end(), context) != 1) {
    throw std::invalid_argument("retained observed-state channel target is not "
                                "registered exactly once");
  }
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
      const auto *current_binding =
          current_binding_.load(std::memory_order_acquire);
      if (binding.value() != current_binding->value()) {
        if (retained_route_bindings_.size() >= 4096U) {
          return core::Result<void>::Err(channel_error(
              "retained_state_route_rebind_capacity_exhausted",
              "native route-state binding history reached its fixed bound"));
        }
        retained_route_bindings_.push_back(
            std::make_unique<const core::RuntimeParticipantBinding>(
                std::move(binding)));
        current_binding_.store(retained_route_bindings_.back().get(),
                               std::memory_order_release);
        notify_render_change();
      }
      return core::Result<void>::Ok();
    }
    if (retained_route_bindings_.size() >= 4096U) {
      return core::Result<void>::Err(channel_error(
          "retained_state_route_rebind_capacity_exhausted",
          "native route-state binding history reached its fixed bound"));
    }
    retained_route_bindings_.push_back(
        std::make_unique<const core::RuntimeParticipantBinding>(
            std::move(binding)));
    const bool was_fresh_state_required =
        fresh_state_required_.load(std::memory_order_acquire);
    fresh_state_required_.store(true, std::memory_order_release);
    auto rebound = publication_.rebind_route_context(std::move(rebound_context),
                                                     transition_time_ns);
    if (!rebound.ok()) {
      retained_route_bindings_.pop_back();
      fresh_state_required_.store(was_fresh_state_required,
                                  std::memory_order_release);
      fresh_state_cv_.notify_all();
      return rebound;
    }
    current_binding_.store(retained_route_bindings_.back().get(),
                           std::memory_order_release);
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
  if (!projector_) {
    return {
        RetainedStateProjectionStatus::kRejected,
        channel_error("component_local_state_has_no_model_projection",
                      "component-local state cannot be seeded from a model")};
  }
  auto state = projector_->current_state(
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
  const auto &context = publication_.query_context();
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
  if (published.published && published.complete &&
      published.velocities_complete) {
    fresh_state_required_.store(false, std::memory_order_release);
    fresh_sample_generation_.fetch_add(1U, std::memory_order_acq_rel);
    record_render_positions(source_joint_names, positions_si);
    fresh_state_cv_.notify_all();
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
  const auto &context = publication_.query_context();
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
  if (!projector_) {
    return {RetainedStateProjectionStatus::kApplied, std::nullopt};
  }
  auto projected = std::move(snapshot.value());
  projected.control_route_id = projection_context_.control_route_id;
  projected.active_control_configuration_id =
      projection_context_.active_control_configuration_id;
  projected.active_control_configuration_generation =
      projection_context_.active_control_configuration_generation;
  return projector_->apply(projected, now_ns, mode);
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
  auto result =
      publication_.current_state(publication_.query_context(), now_ns);
  if (cancellation.stop_requested() || is_expired()) {
    return core::Result<core::ObservedStateSnapshotDTO>::Err(channel_error(
        cancellation.stop_requested() ? "retained_state_read_cancelled"
                                      : "retained_state_read_deadline_expired",
        cancellation.stop_requested()
            ? "native state read was cancelled"
            : "native state read missed its deadline"));
  }
  return result;
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
  std::unique_lock lock(fresh_state_mutex_);
  return fresh_state_cv_.wait_for(lock, timeout, [this, after_generation] {
    return render_change_generation_.load(std::memory_order_acquire) !=
           after_generation;
  });
}

void RetainedObservedStateChannel::record_render_positions(
    const std::span<const std::string> source_joint_names,
    const std::span<const double> positions_si) {
  const auto &canonical_names = publication_.query_context().joint_names;
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
  render_change_generation_.fetch_add(1U, std::memory_order_acq_rel);
  fresh_state_cv_.notify_all();
}

} // namespace manyforge::roboplan_backend
