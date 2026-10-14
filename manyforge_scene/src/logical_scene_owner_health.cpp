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

core::Result<ProjectionRevisionDTO>
LogicalSceneOwner::ConfirmProviderFreshness(
    const core::ProviderFreshnessConfirmationDTO &confirmation) {
  using ConfirmationResult = core::Result<ProjectionRevisionDTO>;
  const auto reject = [](const std::string_view code,
                         const std::string_view message,
                         std::map<std::string, std::string> details = {}) {
    return ConfirmationResult::Err(
        SceneError(code, message, std::move(details)));
  };
  if (!confirmation.valid()) {
    return reject("invalid_request",
                  "provider freshness confirmation is malformed");
  }
  try {
    ValidateBoundedIdentity(confirmation.scene_resource_id.value(),
                            "scene_resource_id");
  } catch (const std::invalid_argument &error) {
    return reject("invalid_request", error.what());
  }
  const auto &scene_resource_id = confirmation.scene_resource_id;
  const auto &observed = confirmation.evidence;
  const auto conflict = [&](const std::string_view reason,
                            const std::string_view message,
                            std::map<std::string, std::string> details = {}) {
    details.emplace("reason", std::string(reason));
    details.emplace("scene_resource_id", scene_resource_id.value());
    return reject("scene_object_state_conflict", message, std::move(details));
  };

  // Same writer exclusion as Commit: compaction, maintenance and every other
  // scene write hold this mutex, and AccessStatus closes writes during
  // bootstrap, maintenance and unresolved/desynchronized projection states.
  std::lock_guard lock(impl_->mutex_);
  ProjectionRevisionDTO active;
  std::optional<SceneObjectSnapshotDTO> object;
  std::vector<core::ProjectionNativeRuntimeBindingDTO> required_bindings;
  {
    const auto &cached = impl_->ReadSceneCommitState();
    const auto &state = cached.durable_state;
    if (!state.active_projection.has_value() ||
        !state.active_scene_snapshot.has_value()) {
      return reject("live_scene_snapshot_unavailable",
                    "logical-scene bootstrap is not finalized");
    }
    if (!AccessStatus(state).scene_writes_open) {
      return reject("live_scene_snapshot_unavailable",
                    "logical-scene projection is unavailable or "
                    "desynchronized");
    }
    if (confirmation.expected_projection_revision !=
        *state.active_projection) {
      return reject(
          "stale_revision",
          "expected composite projection revision is no longer active");
    }
    const auto *found =
        FindSceneObject(*state.active_scene_snapshot, scene_resource_id);
    if (found == nullptr) {
      return reject("object_not_found", "scene resource does not exist",
                    {{"scene_resource_id", scene_resource_id.value()}});
    }
    impl_->PruneProviderFreshnessState(*state.active_scene_snapshot);
    active = *state.active_projection;
    object = *found;
    required_bindings = ActiveNativeRuntimeBindings(state);
  }

  const auto *fixed =
      std::get_if<core::CellFixedSceneObjectPlacementDTO>(&object->placement);
  if (fixed == nullptr) {
    return conflict("provider_freshness_object_attached",
                    "a carried object pose is derived from its attachment");
  }
  if (!SamePose(fixed->pose_in_cell, confirmation.pose_in_cell)) {
    return conflict("provider_freshness_pose_changed",
                    "observed pose differs from the committed pose");
  }
  if (!object->provider_evidence.has_value()) {
    return conflict("provider_freshness_evidence_missing",
                    "committed object has no provider evidence to refresh");
  }
  const auto &committed = *object->provider_evidence;
  if (observed.provider_instance_id != committed.provider_instance_id ||
      observed.provider_manifest_id != committed.provider_manifest_id ||
      observed.provider_manifest_revision !=
          committed.provider_manifest_revision ||
      observed.provider_source_epoch != committed.provider_source_epoch) {
    return conflict("provider_freshness_origin_changed",
                    "observed provider origin differs from the committed "
                    "evidence");
  }
  if (observed.source_freshness_budget_ns !=
      committed.source_freshness_budget_ns) {
    return conflict("provider_freshness_budget_changed",
                    "observed freshness budget differs from the committed "
                    "evidence");
  }

  auto &records = impl_->provider_freshness_records_;
  if (const auto record = records.find(scene_resource_id);
      record != records.end() &&
      (record->second.retained_bundle_id != active.retained_bundle_id ||
       record->second.graph_revision != active.graph_revision ||
       record->second.confirmed_against != *object)) {
    records.erase(record);
  }
  const auto record = records.find(scene_resource_id);
  const auto &latest =
      record == records.end() ? committed : record->second.accepted;
  const auto previous_sequence = std::max(
      latest.provider_source_sequence,
      impl_->ProviderSequenceWatermark(scene_resource_id, observed)
          .value_or(0U));
  if (observed.provider_source_sequence <= previous_sequence) {
    return conflict(
        "provider_freshness_sequence_not_increasing",
        "provider source sequence did not advance",
        {{"previous_provider_source_sequence",
          std::to_string(previous_sequence)},
         {"received_provider_source_sequence",
          std::to_string(observed.provider_source_sequence)}});
  }
  if (observed.received_at_ns < latest.received_at_ns) {
    return conflict("provider_freshness_received_at_regressed",
                    "provider observation is older than the latest accepted "
                    "evidence");
  }

  std::vector<RequiredSceneProjection> projections;
  projections.reserve(required_bindings.size());
  for (const auto &binding : required_bindings) {
    try {
      auto projection =
          ResolveRequiredProjection(
            binding, impl_->projection_resolver_, active);
      if (!projection.preflight_health.healthy_for_target(active)) {
        return reject("live_scene_snapshot_unavailable",
                      "a required native scene projection is not healthy at "
                      "the exact current revision");
      }
      projections.push_back(std::move(projection));
    } catch (const std::exception &error) {
      return reject("live_scene_snapshot_unavailable", error.what());
    } catch (...) {
      return reject("live_scene_snapshot_unavailable",
                    "native scene projection health threw a non-standard "
                    "exception");
    }
  }

  // Every precondition holds: the observation is accepted before any native
  // effect, so a partial fan-out can never be replayed to renew freshness.
  const auto previous_record =
      record == records.end()
          ? std::nullopt
          : std::optional<ProviderFreshnessRecord>(record->second);
  const auto previous_watermark =
      impl_->provider_sequence_watermarks_.contains(scene_resource_id)
          ? std::optional<core::SceneObjectProviderEvidenceDTO>(
                impl_->provider_sequence_watermarks_.at(scene_resource_id))
          : std::nullopt;
  impl_->AdvanceProviderSequenceWatermark(scene_resource_id, observed);
  records.insert_or_assign(
      scene_resource_id,
      ProviderFreshnessRecord{active.retained_bundle_id, active.graph_revision,
                              *object, observed});

  const auto append = [](std::string &list, const std::string_view value) {
    if (!list.empty()) {
      list += ',';
    }
    list += value;
  };
  std::string applied_ordinals;
  std::string applied_runtime_units;
  std::string failed_ordinals;
  std::string failed_runtime_units;
  // A lost acknowledgement or a throwing call has an unknown effect: the
  // projection may already publish the confirmed freshness.
  std::string uncertain_ordinals;
  std::string uncertain_runtime_units;
  std::optional<ErrorDTO> first_failure;
  bool every_failure_unsupported = true;
  for (std::size_t ordinal = 0U; ordinal < projections.size(); ++ordinal) {
    const auto &projection = projections[ordinal];
    std::optional<ErrorDTO> failure;
    try {
      const auto applied =
          projection.projection->ApplyProviderFreshness(confirmation);
      if (!applied.ok()) {
        failure = applied.error();
      }
    } catch (const std::exception &error) {
      failure = SceneError("scene_projection_call_failed", error.what());
    } catch (...) {
      failure = SceneError("scene_projection_call_failed",
                           "native scene projection threw a non-standard "
                           "exception");
    }
    const auto &runtime_unit =
        projection.preflight_health.native_runtime_binding.runtime_unit_id
            .value();
    if (!failure.has_value()) {
      append(applied_ordinals, std::to_string(ordinal));
      append(applied_runtime_units, runtime_unit);
      continue;
    }
    append(failed_ordinals, std::to_string(ordinal));
    append(failed_runtime_units, runtime_unit);
    if (failure->code == "projection_state_uncertain" ||
        failure->code == "scene_projection_call_failed") {
      append(uncertain_ordinals, std::to_string(ordinal));
      append(uncertain_runtime_units, runtime_unit);
    }
    every_failure_unsupported = every_failure_unsupported &&
                                failure->code == "provider_freshness_unsupported";
    if (!first_failure.has_value()) {
      first_failure = std::move(failure);
    }
  }
  if (!first_failure.has_value()) {
    return ConfirmationResult::Ok(active);
  }

  std::map<std::string, std::string> details{
      {"reason", "provider_freshness_projection_failed"},
      {"uncertain_projection_ordinals", uncertain_ordinals},
      {"uncertain_runtime_unit_ids", uncertain_runtime_units},
      {"scene_resource_id", scene_resource_id.value()},
      {"failed_projection_ordinals", failed_ordinals},
      {"failed_runtime_unit_ids", failed_runtime_units},
      {"applied_projection_ordinals", applied_ordinals},
      {"applied_runtime_unit_ids", applied_runtime_units},
      {"projection_error_code", first_failure->code},
      {"projection_error_message", first_failure->message},
  };
  if (applied_ordinals.empty() && every_failure_unsupported) {
    // No freshness path exists and nothing was published: restore the prior
    // state so the caller's ordinary Commit fallback can carry this sequence.
    if (previous_record.has_value()) {
      records.insert_or_assign(scene_resource_id, *previous_record);
    } else {
      records.erase(scene_resource_id);
    }
    if (previous_watermark.has_value()) {
      impl_->provider_sequence_watermarks_.insert_or_assign(
          scene_resource_id, *previous_watermark);
    } else {
      impl_->provider_sequence_watermarks_.erase(scene_resource_id);
    }
    details["reason"] = "provider_freshness_unsupported";
    return reject("provider_freshness_unsupported",
                  "no required native projection supports freshness-only "
                  "publication",
                  std::move(details));
  }
  // The accepted observation stays recorded: projections that applied it are
  // fresh; a projection that definitely refused keeps its older freshness and
  // ages out fail-closed; an uncertain one (lost acknowledgement or throwing
  // call) may or may not have published it, and is reported as uncertain.
  return reject("provider_freshness_projection_failed",
                "a required native projection did not apply the freshness "
                "confirmation",
                std::move(details));
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
