#include "logical_scene_owner_impl.hpp"

namespace manyforge::scene {

ProjectionAggregateHealthDTO
LogicalSceneOwner::ActiveProjectionHealth() const {
  const auto state = [&] {
    std::lock_guard lock(impl_->mutex_);
    return impl_->ReadSceneCommitState().durable_state;
  }();
  if (!state.active_projection.has_value() ||
      !state.active_scene_snapshot.has_value() || !state.accepted.has_value()) {
    throw LogicalSceneOwnerConflictError(
        "active native projection health requires finalized bootstrap");
  }
  core::RequiredNativeProjectionSetSourceDTO source =
      [&]() -> core::RequiredNativeProjectionSetSourceDTO {
    if (const auto *maintenance = state.LatestMaintenanceInitialization();
        maintenance != nullptr &&
        maintenance->publication_receipt.has_value()) {
      return MaintenanceProjectionSetSourceDTO{
          maintenance->apply_id, maintenance->apply_generation,
          *state.active_projection};
    }
    if (!state.migrated_structural_completions.empty()) {
      const auto &completion = state.migrated_structural_completions.back();
      return MaintenanceProjectionSetSourceDTO{
          completion.apply_id, completion.apply_generation, completion.target};
    }
    return BootstrapProjectionSetSourceDTO{state.accepted->bootstrap_id,
                                           state.accepted->bootstrap_generation,
                                           state.accepted->target};
  }();

  const auto bindings = ActiveNativeRuntimeBindings(state);
  std::vector<core::ProjectionHealthDTO> health;
  health.reserve(bindings.size());
  std::optional<ErrorDTO> aggregate_error;
  const auto access = AccessStatus(state);
  for (const auto &binding : bindings) {
    try {
      auto observed =
          ResolveRequiredProjection(
            binding, impl_->projection_resolver_, *state.active_projection);
      if (!observed.preflight_health.healthy_for_target(
              *state.active_projection)) {
        throw LogicalSceneOwnerConflictError(
            "native projection is not healthy at the active revision");
      }
      health.push_back(std::move(observed.preflight_health));
    } catch (const std::exception &error) {
      ErrorDTO unavailable{
          "active_native_projection_unavailable", error.what(), {}};
      health.push_back(core::ProjectionHealthDTO{
          binding,
          1U,
          ProjectionHealthState::kUnavailable,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          unavailable,
      });
      aggregate_error = std::move(unavailable);
    } catch (...) {
      ErrorDTO unavailable{
          "active_native_projection_unavailable",
          "native projection health threw a non-standard exception",
          {}};
      health.push_back(core::ProjectionHealthDTO{
          binding,
          1U,
          ProjectionHealthState::kUnavailable,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          unavailable,
      });
      aggregate_error = std::move(unavailable);
    }
  }
  if (!access.scene_writes_open || !access.projection_leases_open) {
    aggregate_error = ErrorDTO{
        "active_native_projection_initialization_pending",
        "active native projection initialization or scene transaction is incomplete",
        {}};
  }
  ProjectionAggregateHealthDTO aggregate{
      std::move(source),
      aggregate_error.has_value() ? ProjectionHealthState::kUnavailable
                                  : ProjectionHealthState::kHealthy,
      std::move(health),
      std::move(aggregate_error),
  };
  if (!aggregate.valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "active native projection set cannot form aggregate health");
  }
  return aggregate;
}

RequestOwnerEpoch LogicalSceneOwner::CurrentRequestOwnerEpoch() const {
  std::lock_guard lock(impl_->mutex_);
  const auto &cached = impl_->ReadSceneCommitState();
  const auto &state = cached.durable_state;
  if (!state.active_projection.has_value() ||
      !state.active_scene_snapshot.has_value() ||
      !cached.transaction_state.has_value()) {
    throw LogicalSceneOwnerConflictError(
        "logical-scene request owner epoch requires finalized bootstrap");
  }
  return cached.transaction_state->request_owner_epoch;
}

ProjectionRevisionDTO
LogicalSceneOwner::CurrentProjectionRevision() const {
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadSceneCommitState().durable_state;
  if (!AccessStatus(state).projection_leases_open ||
      !state.active_projection.has_value()) {
    throw LogicalSceneOwnerConflictError(
        "live logical-scene projection is unavailable");
  }
  return *state.active_projection;
}

std::shared_ptr<const LogicalSceneSnapshotDTO>
LogicalSceneOwner::LogicalSnapshot() const {
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadSceneCommitState().durable_state;
  if (!AccessStatus(state).projection_leases_open ||
      !state.active_scene_snapshot.has_value()) {
    throw LogicalSceneOwnerConflictError(
        "live logical-scene snapshot is unavailable");
  }
  return std::make_shared<const LogicalSceneSnapshotDTO>(
      *state.active_scene_snapshot);
}

std::vector<SceneObjectSnapshotDTO>
LogicalSceneOwner::GetSceneObjects() const {
  return LogicalSnapshot()->objects;
}

SceneObjectStateDTO LogicalSceneOwner::GetObjectState(
    const core::SceneResourceId &scene_resource_id) const {
  ValidateBoundedIdentity(scene_resource_id.value(), "scene_resource_id");
  const auto snapshot = LogicalSnapshot();
  const auto found = std::lower_bound(
      snapshot->objects.begin(), snapshot->objects.end(), scene_resource_id,
      [](const SceneObjectSnapshotDTO &object,
         const core::SceneResourceId &id) {
        return object.scene_resource_id < id;
      });
  if (found == snapshot->objects.end() ||
      found->scene_resource_id != scene_resource_id) {
    return {scene_resource_id, false, false, std::nullopt, std::nullopt};
  }
  const auto *carried =
      std::get_if<core::CarriedSceneObjectPlacementDTO>(&found->placement);
  return {
      scene_resource_id,
      true,
      carried != nullptr,
      carried == nullptr ? std::nullopt
                         : std::optional<core::CarriedObjectAttachmentId>(
                               carried->carried_object_attachment_id),
      found->provider_evidence,
  };
}

core::Result<core::Pose3DTO> LogicalSceneOwner::GetObjectPose(
    const core::SceneResourceId &scene_resource_id) const {
  ValidateBoundedIdentity(scene_resource_id.value(), "scene_resource_id");
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadSceneCommitState().durable_state;
  if (!AccessStatus(state).projection_leases_open ||
      !state.active_projection.has_value() ||
      !state.active_scene_snapshot.has_value()) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError("live_scene_snapshot_unavailable",
                   "logical-scene projection is unavailable"));
  }
  const auto snapshot = std::make_shared<const LogicalSceneSnapshotDTO>(
      *state.active_scene_snapshot);
  const auto found = std::lower_bound(
      snapshot->objects.begin(), snapshot->objects.end(), scene_resource_id,
      [](const SceneObjectSnapshotDTO &object,
         const core::SceneResourceId &id) {
        return object.scene_resource_id < id;
      });
  if (found == snapshot->objects.end() ||
      found->scene_resource_id != scene_resource_id) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError("object_not_found", "scene resource does not exist",
                   {{"scene_resource_id", scene_resource_id.value()}}));
  }
  const auto *fixed =
      std::get_if<core::CellFixedSceneObjectPlacementDTO>(&found->placement);
  if (fixed != nullptr) {
    return core::Result<core::Pose3DTO>::Ok(fixed->pose_in_cell);
  }
  const auto *carried =
      std::get_if<core::CarriedSceneObjectPlacementDTO>(&found->placement);
  if (carried == nullptr) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError("scene_object_state_conflict",
                   "scene resource has an unknown placement state",
                   {{"scene_resource_id", scene_resource_id.value()}}));
  }
  const auto attachment =
      std::lower_bound(snapshot->carried_object_attachments.begin(),
                       snapshot->carried_object_attachments.end(),
                       carried->carried_object_attachment_id,
                       [](const core::CarriedObjectAttachmentDTO &candidate,
                          const core::CarriedObjectAttachmentId &id) {
                         return candidate.carried_object_attachment_id < id;
                       });
  if (attachment == snapshot->carried_object_attachments.end() ||
      attachment->carried_object_attachment_id !=
          carried->carried_object_attachment_id ||
      attachment->carried_object_attachment_revision !=
          carried->carried_object_attachment_revision) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError("scene_object_state_conflict",
                   "carried scene resource has no exact attachment row",
                   {{"scene_resource_id", scene_resource_id.value()}}));
  }

  std::vector<RequiredSceneProjection> projections;
  try {
    const auto bindings = ActiveNativeRuntimeBindings(state);
    projections.reserve(bindings.size());
    for (const auto &binding : bindings) {
      auto projection =
          ResolveRequiredProjection(
            binding, impl_->projection_resolver_, *state.active_projection);
      if (!projection.preflight_health.healthy_for_target(
              *state.active_projection)) {
        throw LogicalSceneOwnerConflictError(
            "native projection is not healthy at the carried-object "
            "revision");
      }
      projections.push_back(std::move(projection));
    }
    const auto resolution = ResolveFrameAcrossProjections(
        core::SceneFrameResolutionSelectorDTO{attachment->carrier_frame_ref},
        *state.active_projection, projections,
        "object-pose-" + scene_resource_id.value());
    if (!resolution.has_value()) {
      throw LogicalSceneOwnerIntegrityError(
          "carried-object frame resolution returned no receipt");
    }
    return core::Result<core::Pose3DTO>::Ok(core::multiply_poses(
        resolution->frame_pose_in_cell, attachment->pose_in_carrier_frame));
  } catch (const SceneRequestRejectionError &error) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError(error.code(), error.what(), error.details()));
  } catch (const std::exception &error) {
    return core::Result<core::Pose3DTO>::Err(
        SceneError("live_scene_snapshot_unavailable", error.what(),
                   {{"scene_resource_id", scene_resource_id.value()}}));
  }
}

CanonicalEmptySceneAccessStatus
LogicalSceneOwner::ReadAccessStatus() const {
  std::lock_guard lock(impl_->mutex_);
  auto status = AccessStatus(impl_->ReadSceneCommitState().durable_state);
  if (!status.valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "durable state cannot form a valid access-gate status");
  }
  return status;
}

std::optional<LogicalSceneSnapshotRoot>
LogicalSceneOwner::ReadActiveSnapshot() const {
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadSceneCommitState().durable_state;
  if (!state.active_projection.has_value() ||
      !state.active_scene_snapshot.has_value()) {
    return std::nullopt;
  }
  if (state.active_scene_snapshot->revision !=
          state.active_projection->scene_revision ||
      state.active_scene_snapshot->cell_id != impl_->fixed_query_.cell_id) {
    throw LogicalSceneOwnerIntegrityError(
        "active projection does not identify the durable logical-scene "
        "snapshot");
  }
  return LogicalSceneSnapshotRoot(
      std::make_shared<const LogicalSceneSnapshotDTO>(
          *state.active_scene_snapshot));
}

} // namespace manyforge::scene
