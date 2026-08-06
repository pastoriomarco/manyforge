#include <manyforge_roboplan/retained_observed_state_channel.hpp>

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] core::ObservedRobotStateSourceKind numeric_source_kind(
    const core::ObservedStateSourceKind source) noexcept {
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

[[nodiscard]] core::ObservedRobotStateAuthorityProfile authority_profile(
    const core::StateAuthorityClass authority) {
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
publication_config(
    const core::RuntimeParticipantBinding& binding,
    const RetainedObservedStateChannelConfig& config) {
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
  return core::AddressedObservedStatePublicationConfig{
      std::move(context.value()),
      {core::ObservedRobotStateSourceConfig{
          numeric_source_kind(config.source_kind), config.source_id,
          profile, config.freshness_budget_ns,
          "retained_native_state_authority"}},
      config.authority_generation,
      256U};
}

[[nodiscard]] core::ErrorDTO channel_error(std::string reason,
                                           std::string message) {
  return core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
      std::move(message), {{"reason", std::move(reason)}}};
}

[[nodiscard]] bool publication_backpressured(
    const std::string_view reason) noexcept {
  return reason == "source_writer_busy" ||
         reason == "publication_slots_exhausted";
}

[[nodiscard]] double position_value(
    const core::CoordinatePositionDTO& position) noexcept {
  if (const auto* angular =
          std::get_if<core::AngularPositionDTO>(&position)) {
    return angular->value_rad;
  }
  return std::get<core::LinearPositionDTO>(position).value_m;
}

}  // namespace

RetainedObservedStateChannel::RetainedObservedStateChannel(
    core::RuntimeParticipantBinding binding,
    std::shared_ptr<RetainedKinematicStateProjector> projector,
    RetainedObservedStateChannelConfig config)
    : binding_(std::move(binding)),
      projector_(std::move(projector)),
      config_(std::move(config)),
      publication_(publication_config(binding_, config_)) {
  if (!projector_) {
    throw std::invalid_argument(
        "retained observed-state channel requires a projector");
  }
  const auto& context = publication_.query_context();
  const auto contexts = projector_->query_contexts();
  if (std::count(contexts.begin(), contexts.end(), context) != 1) {
    throw std::invalid_argument(
        "retained observed-state channel target is not registered exactly once");
  }
}

RetainedStateProjectionResult
RetainedObservedStateChannel::initialize_from_projection(
    const std::int64_t timestamp_ns) {
  auto state = projector_->current_state(
      publication_.query_context().control_binding_id,
      publication_.query_context().control_binding_revision);
  if (!state.ok()) {
    return {RetainedStateProjectionStatus::kFailed, state.error()};
  }
  std::vector<double> positions;
  positions.reserve(state.value().positions.size());
  for (const auto& position : state.value().positions) {
    positions.push_back(position_value(position));
  }
  std::vector<double> velocities(positions.size(), 0.0);
  return publish_and_project(positions, velocities, timestamp_ns, timestamp_ns,
                             RetainedStateProjectionMode::kBlocking);
}

RetainedStateProjectionResult
RetainedObservedStateChannel::publish_and_project(
    const std::span<const double> positions_si,
    const std::span<const double> velocities_si,
    const std::int64_t source_timestamp_ns,
    const std::int64_t received_timestamp_ns,
    const RetainedStateProjectionMode mode) {
  auto published = publish_state(positions_si, velocities_si,
                                 source_timestamp_ns,
                                 received_timestamp_ns);
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
  const auto& context = publication_.query_context();
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
    return {publication_backpressured(reason)
                ? RetainedStateProjectionStatus::kBusy
                : RetainedStateProjectionStatus::kRejected,
            channel_error(reason,
                          "native state publication was not committed")};
  }
  return {RetainedStateProjectionStatus::kApplied, std::nullopt};
}

RetainedStateProjectionResult RetainedObservedStateChannel::project_latest(
    const std::int64_t now_ns, const RetainedStateProjectionMode mode) {
  const auto& context = publication_.query_context();
  auto snapshot = publication_.current_state(context, now_ns);
  if (!snapshot.ok()) {
    return {RetainedStateProjectionStatus::kFailed, snapshot.error()};
  }
  return projector_->apply(snapshot.value(), now_ns, mode);
}

core::Result<core::ObservedStateSnapshotDTO>
RetainedObservedStateChannel::current_state(const std::int64_t now_ns) const {
  return publication_.current_state(publication_.query_context(), now_ns);
}

core::Result<core::ControlledStateDTO>
RetainedObservedStateChannel::current_controlled_state() const {
  return projector_->current_state(
      publication_.query_context().control_binding_id,
      publication_.query_context().control_binding_revision);
}

}  // namespace manyforge::roboplan_backend
