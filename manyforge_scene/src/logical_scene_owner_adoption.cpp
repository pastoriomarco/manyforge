#include "logical_scene_owner_impl.hpp"

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

using ProjectionProviderBindingKey =
    std::pair<core::CapabilityProviderBindingId,
              core::CapabilityProviderBindingRevision>;

using ProjectionProviderRuntimeRequirements =
    std::map<ProjectionProviderBindingKey, core::RuntimeUnitId>;

[[nodiscard]] ProjectionProviderRuntimeRequirements
NativeSceneProviderBindings(const RetainedBundleDTO &bundle) {
  std::set<core::RuntimeUnitId> declared_runtime_units;
  for (const auto &runtime_unit : bundle.cell_graph.runtime_units) {
    if (!declared_runtime_units.insert(runtime_unit.runtime_unit_id).second) {
      throw LogicalSceneOwnerIntegrityError(
          "verified retained bundle repeats a runtime-unit identity");
    }
  }
  ProjectionProviderRuntimeRequirements result;
  for (const auto &binding : bundle.capability_provider_bindings) {
    if (!core::RequiresNativeSceneProjection(
            binding.capability_id, binding.capability_definition_revision)) {
      continue;
    }
    std::set<core::RuntimeUnitId> runtime_sources;
    for (const auto &endpoint : binding.endpoint_bindings) {
      if (endpoint.source_kind !=
          core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *runtime_unit =
          std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
      if (runtime_unit == nullptr || runtime_unit->empty()) {
        throw LogicalSceneOwnerIntegrityError(
            "verified native provider has a malformed runtime endpoint");
      }
      runtime_sources.insert(*runtime_unit);
    }
    if (runtime_sources.size() != 1U ||
        !declared_runtime_units.contains(*runtime_sources.begin())) {
      throw LogicalSceneOwnerIntegrityError(
          "verified native provider does not resolve one declared runtime "
          "unit");
    }
    const auto inserted = result.emplace(
        ProjectionProviderBindingKey{
            binding.capability_provider_binding_id,
            binding.capability_provider_binding_revision},
        *runtime_sources.begin());
    if (!inserted.second) {
      throw LogicalSceneOwnerIntegrityError(
          "verified retained bundle repeats a native scene provider binding");
    }
  }
  return result;
}

template <typename Row>
void BindProjection(Row &statement, const int first_column,
                    const ProjectionRevisionDTO &projection) {
  statement.Bind(first_column, projection.retained_bundle_id.value());
  statement.Bind(first_column + 1, projection.graph_revision.value());
  statement.Bind(first_column + 2,
                 projection.scene_revision.scene_id.value());
  statement.Bind(first_column + 3,
                 CheckedSafeCounter(projection.scene_revision.revision_id,
                                    "scene revision"));
  statement.Bind(first_column + 4,
                 projection.scene_revision.content_hash.value());
}

ProjectionMaintenanceInitializationStatusDTO MaintenanceStatus(
    const MaintenanceInitializationState &state,
    const ProjectionMaintenanceInitializationState visible_state) {
  ProjectionMaintenanceInitializationStatusDTO result{
      state.apply_id,
      state.apply_generation,
      visible_state,
      state.predecessor,
      state.target,
      state.authoritative_snapshot.identity(),
      state.bindings,
      state.publication_receipt,
      state.first_error,
      state.effects_unknown,
      state.initial_baseline_installed,
  };
  if (!result.valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "maintenance initialization cannot form a valid status");
  }
  return result;
}

ContentDigest InitialSceneRequestDigest(
    const ProjectionMaintenanceInitializationRequestDTO &request) {
  std::string encoded;
  for (std::size_t index = 0U; index < request.initial_scene_objects.size();
       ++index) {
    const SceneTransactionRequestDTO item{
        // Digest input only: the Apply number in the key, the object's index
        // in the label (both formerly in the request id and epoch).
        core::PublicRequestContextDTO{
            core::RequestKey::Make(
                core::RequestKeyClass::kInternal, 0U,
                static_cast<std::uint64_t>(core::RequestKeyMinter::kManager),
                request.apply_id.value()),
            "maintenance_initial_scene." + std::to_string(index)},
        {}, request.expected_active_projection_revision,
        request.initial_scene_objects[index], std::nullopt, std::nullopt,
        std::nullopt};
    const auto part = logical_scene_codec_internal::EncodeRequest(item);
    if (part.size() > kMaximumLogicalSceneTransactionBytes ||
        encoded.size() > kMaximumLogicalSceneTransactionBytes - part.size()) {
      throw LogicalSceneOwnerLimitError(
          "initial Scene input exceeds the bounded snapshot extent");
    }
    encoded += part;
  }
  return ContentDigest(manyforge::content_identity::sha256_digest(encoded));
}

LogicalSceneSnapshotDTO BuildInitialSceneBaseline(
    const ProjectionMaintenanceInitializationRequestDTO &request,
    const LogicalSceneSnapshotDTO &predecessor,
    const ProjectionRevisionDTO &provisional,
    const std::vector<RequiredSceneProjection> &projections) {
  if (predecessor.revision != provisional.scene_revision ||
      predecessor.cell_id != request.candidate_retained_bundle.cell_graph.cell_id ||
      predecessor.revision.revision_id >= kMaximumSafeIntegral ||
      predecessor.topology_epoch >= kMaximumSafeIntegral) {
    throw LogicalSceneOwnerConflictError(
        "initial Scene baseline has no exact predecessor lineage");
  }
  auto candidate = predecessor;
  candidate.objects.clear();
  candidate.carried_object_attachments.clear();
  candidate.revision.revision_id += 1U;
  candidate.topology_epoch += 1U;
  const auto request_digest = InitialSceneRequestDigest(request);
  for (const auto &operation : request.initial_scene_objects) {
    std::optional<core::SceneFrameResolutionReceiptDTO> resolution;
    if (!std::holds_alternative<core::CellSceneFrameRefDTO>(
            operation.frame_ref)) {
      const auto *component =
          std::get_if<core::ComponentSceneFrameRefDTO>(&operation.frame_ref);
      if (component == nullptr) {
        throw SceneRequestRejectionError(
            "missing_reference",
            "initial Scene object frame is not a supported candidate frame",
            {{"scene_resource_id", operation.scene_resource_id.value()}});
      }
      resolution = ResolveFrameAcrossProjections(
          core::SceneFrameResolutionSelectorDTO{
              std::in_place_type<core::SceneFrameRefDTO>,
              core::SceneFrameRefDTO{*component}},
          provisional, projections,
          "initial-scene-" + operation.scene_resource_id.value());
    }
    const auto pose = NormalizeScenePose(
        operation.frame_ref, operation.pose, predecessor, provisional,
        request_digest, resolution);
    candidate.objects.push_back(core::SceneObjectSnapshotDTO{
        operation.scene_resource_id, {operation.shape},
        core::CellFixedSceneObjectPlacementDTO{candidate.cell_id,
                                               pose.pose_in_cell},
        std::nullopt, {}, operation.mass_kg});
  }
  for (std::size_t index = 0U; index < candidate.objects.size(); ++index) {
    candidate.objects[index].support_contacts = NormalizeSupportContacts(
        candidate.objects[index].scene_resource_id,
        request.initial_scene_objects[index].support_contacts, candidate);
  }
  candidate.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(candidate);
  if (!candidate.locally_valid() ||
      logical_scene_codec_internal::EncodeSnapshot(candidate).size() > 131072U) {
    throw SceneRequestRejectionError(
        "invalid_request", "initial Scene baseline is invalid or too large",
        {});
  }
  static_cast<void>(ProjectEffectivePolicy(candidate, projections));
  return candidate;
}

ProjectionMaintenanceInitializationStatusDTO MaintenanceStatus(
    const MaintenanceInitializationState &state) {
  return MaintenanceStatus(
      state,
      state.publication_receipt.has_value()
          ? ProjectionMaintenanceInitializationState::kCompleted
          : state.result_state.value_or(
                ProjectionMaintenanceInitializationState::kInitializing));
}

// Records the result row in memory, as the file returns it, and appends its
// insert.
void InsertMaintenanceResult(
    SceneStoreMemory &memory, MaintenanceInitializationState &row,
    persistence::SqliteRecord &result, const std::uint64_t sequence,
    const ProjectionMaintenanceInitializationState state,
    const std::optional<ErrorDTO> &error, const bool effects_unknown) {
  const char *stored_state = nullptr;
  switch (state) {
  case ProjectionMaintenanceInitializationState::kInitialized:
    stored_state = "initialized";
    break;
  case ProjectionMaintenanceInitializationState::kFailed:
    stored_state = "failed";
    break;
  case ProjectionMaintenanceInitializationState::kInterrupted:
    stored_state = "interrupted";
    break;
  case ProjectionMaintenanceInitializationState::kInitializing:
  case ProjectionMaintenanceInitializationState::kCompleted:
    throw LogicalSceneOwnerIntegrityError(
        "invalid durable maintenance result state");
  }
  SizedRow inserted(
      result,
      "INSERT INTO logical_scene_maintenance_initialization_results("
      "initialization_sequence,state,first_error_code,first_error_message,"
      "effects_unknown) VALUES(?,?,?,?,?)");
  inserted.Bind(1, CheckedSafeCounter(sequence, "initialization sequence"));
  inserted.Bind(2, std::string_view(stored_state));
  if (error.has_value()) {
    inserted.Bind(3, error->code);
    inserted.Bind(4, error->message);
  } else {
    inserted.BindNull(3);
    inserted.BindNull(4);
  }
  inserted.Bind(5, std::int64_t{effects_unknown ? 1 : 0});
  memory.maintenance_file_bytes[sequence] += SceneRowFileBytes(
      inserted.record_bytes(),
      SceneTable("logical_scene_maintenance_initialization_results"));
  row.result_state = state;
  row.effects_unknown = effects_unknown;
  if (error.has_value()) {
    row.first_error = ErrorDTO{error->code, error->message, {}};
  }
}

// The retained initialization an operation continues; the maintenance
// operation lock keeps it from being evicted meanwhile.
MaintenanceInitializationState &MaintenanceRow(SceneStoreMemory &memory,
                                               const std::uint64_t sequence) {
  return *std::find_if(memory.maintenance_initializations.begin(),
                       memory.maintenance_initializations.end(),
                       [&](const MaintenanceInitializationState &row) {
                         return row.initialization_sequence == sequence;
                       });
}

// Records the completion of `found` with `receipt` and appends its insert and
// the repair watermark. Memory holds the row as the file returns it: the
// receipt is formed from the stored columns and the row's own target.
void InsertMaintenanceCompletion(SceneStoreMemory &memory,
                                 persistence::SqliteRecord &insert,
                                 const MaintenanceInitializationState &found,
                                 const ActiveBundlePublicationReceiptDTO &receipt) {
  SizedRow completion(
      insert,
      "INSERT INTO logical_scene_maintenance_initialization_completions("
      "initialization_sequence,expected_previous_publication_generation,"
      "publication_generation,private_materialization_content_digest,"
      "durable_public_pointer_content_digest) VALUES(?,?,?,?,?)");
  completion.Bind(1, CheckedSafeCounter(found.initialization_sequence,
                                        "initialization sequence"));
  const auto previous_generation = CheckedSafeCounter(
      receipt.request.expected_previous_publication_generation->value(),
      "previous publication generation");
  completion.Bind(2, previous_generation);
  const auto publication_generation = CheckedSafeCounter(
      receipt.publication_generation.value(), "publication generation");
  completion.Bind(3, publication_generation);
  completion.Bind(4,
                  receipt.request.private_materialization_content_digest.value());
  completion.Bind(5, receipt.durable_public_pointer_content_digest.value());
  memory.maintenance_file_bytes[found.initialization_sequence] +=
      SceneRowFileBytes(
          completion.record_bytes(),
          SceneTable("logical_scene_maintenance_initialization_completions"));
  if (found.repaired_transaction_sequence.has_value()) {
    insert.Statement(
        "UPDATE logical_scene_history_state SET "
        "recovered_through_transaction_sequence=? WHERE singleton=1 AND "
        "recovered_through_transaction_sequence<=?");
    insert.Bind(1, CheckedSafeCounter(*found.repaired_transaction_sequence,
                                      "repaired transaction sequence"));
    insert.Bind(2, CheckedSafeCounter(*found.repaired_transaction_sequence,
                                      "repaired transaction sequence"));
  }
  auto &row = MaintenanceRow(memory, found.initialization_sequence);
  row.publication_receipt = ActiveBundlePublicationReceiptDTO{
      ActiveBundlePublicationRequestDTO{
          NamedApplyPublicationTransactionDTO{row.apply_id,
                                              row.apply_generation},
          row.target,
          row.target.retained_bundle_id,
          row.target.graph_revision,
          ActiveBundlePublicationGeneration(
              static_cast<std::uint64_t>(previous_generation)),
          receipt.request.private_materialization_content_digest,
      },
      ActiveBundlePublicationGeneration(
          static_cast<std::uint64_t>(publication_generation)),
      receipt.durable_public_pointer_content_digest,
  };
  row.result_state = ProjectionMaintenanceInitializationState::kCompleted;
  if (found.repaired_transaction_sequence.has_value()) {
    auto &recovered = memory.history.recovered_through_transaction_sequence;
    recovered = std::max(recovered, *found.repaired_transaction_sequence);
  }
}

// The code and message of an adoption whose Apply ended before its
// publication.
constexpr std::string_view kAdoptionEndedWithoutPublicationCode =
    "scene_adoption_ended_without_publication";
constexpr std::string_view kAdoptionEndedWithoutPublicationMessage =
    "the Apply ended before its publication; the Scene keeps its previous "
    "configuration";

// A new initialization row as the file returns it: no result yet, and an
// uninstalled baseline when its request is recorded.
MaintenanceInitializationState NewMaintenanceInitialization(
    const std::uint64_t sequence, const ApplyId &apply_id,
    const ApplyGeneration &apply_generation,
    const ProjectionRevisionDTO &predecessor,
    const ProjectionRevisionDTO &target, const std::string &snapshot_json,
    std::vector<core::ProjectionExecutionHostBindingDTO> bindings,
    const std::optional<std::uint64_t> repaired_transaction_sequence,
    const std::optional<ContentDigest> &baseline_request_digest) {
  return {
      sequence,
      apply_id,
      apply_generation,
      predecessor,
      target,
      logical_scene_codec_internal::DecodeSnapshot(snapshot_json),
      std::move(bindings),
      std::nullopt,
      std::nullopt,
      false,
      std::nullopt,
      repaired_transaction_sequence,
      baseline_request_digest,
      !baseline_request_digest.has_value(),
      std::nullopt,
      false,
      false,
  };
}

// Appends the intent and binding inserts of one initialization.
void InsertMaintenanceIntent(
    SceneStoreMemory &memory, persistence::SqliteRecord &insert,
    const std::uint64_t sequence,
    const ApplyId &apply_id, const std::int64_t apply_generation,
    const ProjectionRevisionDTO &predecessor,
    const ProjectionRevisionDTO &target, const std::string &snapshot_json,
    const std::vector<core::ProjectionExecutionHostBindingDTO> &bindings,
    const std::optional<std::uint64_t> repaired_transaction_sequence) {
  SizedRow intent(
      insert,
      "INSERT INTO logical_scene_maintenance_initialization_intents("
      "initialization_sequence,apply_id,apply_generation,"
      "predecessor_retained_bundle_id,predecessor_graph_revision,"
      "predecessor_scene_id,predecessor_scene_revision_id,"
      "predecessor_scene_content_hash,target_retained_bundle_id,"
      "target_graph_revision,target_scene_id,target_scene_revision_id,"
      "target_scene_content_hash,authoritative_snapshot_json,"
      "native_binding_count,repaired_transaction_sequence) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  intent.Bind(1, static_cast<std::int64_t>(sequence));
  intent.Bind(2, static_cast<std::int64_t>(apply_id.value()));
  intent.Bind(3, apply_generation);
  BindProjection(intent, 4, predecessor);
  BindProjection(intent, 9, target);
  intent.Bind(14, snapshot_json);
  intent.Bind(15, static_cast<std::int64_t>(bindings.size()));
  if (repaired_transaction_sequence.has_value()) {
    intent.Bind(16, static_cast<std::int64_t>(*repaired_transaction_sequence));
  } else {
    intent.BindNull(16);
  }
  auto &file_bytes = memory.maintenance_file_bytes[sequence];
  file_bytes += SceneRowFileBytes(
      intent.record_bytes(),
      SceneTable("logical_scene_maintenance_initialization_intents"));
  for (std::size_t index = 0U; index < bindings.size(); ++index) {
    const auto &binding = bindings[index];
    SizedRow row(
        insert,
        "INSERT INTO logical_scene_maintenance_initialization_bindings("
        "initialization_sequence,binding_ordinal,provider_binding_id,"
        "provider_binding_revision,runtime_unit_id,runtime_epoch) "
        "VALUES(?,?,?,?,?,?)");
    row.Bind(1, static_cast<std::int64_t>(sequence));
    row.Bind(2, static_cast<std::int64_t>(index));
    row.Bind(3, binding.provider_binding_id.value());
    row.Bind(4, binding.provider_binding_revision.value());
    row.Bind(5, binding.runtime_unit_id.value());
    row.Bind(6, binding.runtime_epoch.value());
    file_bytes += SceneRowFileBytes(
        row.record_bytes(),
        SceneTable("logical_scene_maintenance_initialization_bindings"));
  }
}

bool CandidateBindingsMatch(
    const RetainedBundleDTO &bundle,
    const std::vector<core::ProjectionExecutionHostBindingDTO> &bindings) {
  const auto requirements = NativeSceneProviderBindings(bundle);
  if (requirements.size() != bindings.size()) {
    return false;
  }
  for (const auto &binding : bindings) {
    const auto found = requirements.find(
        {binding.provider_binding_id, binding.provider_binding_revision});
    if (found == requirements.end() ||
        found->second != binding.runtime_unit_id) {
      return false;
    }
  }
  return true;
}

const core::ApplyFenceOwnerDTO &AdoptionOwner(const QualifiedSceneConfigurationAdoptionRequest &request) {
  return request.authority.owner;
}

const core::CellAdmissionClaimDTO &AdoptionClaim(const QualifiedSceneConfigurationAdoptionRequest &request) {
  return request.authority.held_cell_claim;
}

const core::ConfigurationFenceStatusQueryDTO &AdoptionFence(
    const QualifiedSceneConfigurationAdoptionRequest &request) {
  return request.authority.held_fence_query;
}

bool AdoptionAuthorityValid(const QualifiedSceneConfigurationAdoptionRequest &request) {
  const auto &authority = request.authority;
  const auto &claim = authority.held_cell_claim;
  const auto &intent = claim.intent;
  if (!authority.owner.valid() ||
      !authority.held_fence_query.valid() ||
      authority.held_fence_query.owner_transaction != authority.owner ||
      !core::ValidateCellAdmissionRecoveryReceiptQuery({intent, claim, std::nullopt}).ok()) {
    return false;
  }
  if (intent.claim_kind == core::CellAdmissionClaimKindDTO::kManagerApply &&
      intent.owner_role == core::ApplicationRole::kManager) {
    return intent.owner_transaction_id ==
               core::CellAdmissionOwnerTransactionId(authority.owner.apply_id) &&
           intent.scope.resources.empty() && intent.scope.configurations.empty() &&
           !intent.program_cycle_evidence && !intent.intervention_evidence && !intent.program_node_evidence;
  }
  // Composer's complete actual stored authoring parent is a different request.
  // Its identity/payload are not fabricated to match Manager's intent digest.
  return intent.claim_kind == core::CellAdmissionClaimKindDTO::kAuthoringMutation &&
         intent.owner_role == core::ApplicationRole::kAuthoringOwner;
}

bool AdoptionReceiptValid(const QualifiedSceneConfigurationAdoptionRequest &,
                          const core::CellAdmissionRecoveryReceiptDTO &receipt) {
  return core::ValidateCellAdmissionRecoveryReceipt(receipt).ok();
}

bool LiveAdoptionMatches(const std::optional<QualifiedSceneConfigurationAdoptionRequest> &live,
                         const QualifiedSceneConfigurationAdoptionRequest &request) {
  return live.has_value() && *live == request;
}

// Apply binds the candidate Deployment and the Scene identity only. The
// target takes the Scene revision actually captured (the latest at Stage or
// Begin); the request's revision is evidence of the moment of the request.
template <typename Request>
ProjectionRevisionDTO AdoptionTarget(
    const Request &request, const core::SceneRevision &scene_revision) {
  return {request.candidate_retained_bundle.retained_bundle_id,
          request.candidate_retained_bundle.cell_graph.graph_revision,
          scene_revision};
}

[[nodiscard]] bool SameSnapshotScene(
    const core::LogicalSceneSnapshotIdentityDTO &left,
    const core::LogicalSceneSnapshotIdentityDTO &right) noexcept {
  return left.cell_id == right.cell_id &&
         left.revision.scene_id == right.revision.scene_id;
}

template <typename Request>
void ValidateAdoptionRequest(const Request &request,
                             const CellId &cell_id) {
  const auto &predecessor = request.expected_predecessor_publication;
  const char *failed_guard = nullptr;
  const auto invalid = [&](bool condition, const char *guard) {
    if (condition) {
      failed_guard = guard;
    }
    return condition;
  };
  if (invalid(!AdoptionAuthorityValid(request), "parent_authority") ||
      invalid(!predecessor.valid(), "predecessor_publication") ||
      invalid(!request.expected_active_projection_revision.valid(),
              "active_projection") ||
      invalid(!request.expected_snapshot_identity.valid(), "snapshot_identity") ||
      invalid(request.expected_snapshot_identity.cell_id != cell_id,
              "snapshot_cell") ||
      invalid(request.expected_active_projection_revision.scene_revision !=
                  request.expected_snapshot_identity.revision,
              "snapshot_revision") ||
      invalid(predecessor.request.candidate_retained_bundle_id !=
                  request.expected_active_projection_revision.retained_bundle_id,
              "predecessor_bundle") ||
      invalid(predecessor.request.candidate_graph_revision !=
                  request.expected_active_projection_revision.graph_revision,
              "predecessor_graph") ||
      invalid(request.candidate_retained_bundle.cell_graph.cell_id != cell_id,
              "candidate_cell") ||
      invalid(!AdoptionTarget(request, request.expected_snapshot_identity.revision).valid(),
              "target_projection") ||
      invalid(request.execution_host_bindings.size() > kMaximumNativeProjectionBindings,
              "native_bindings_limit")) {
    throw std::invalid_argument(
        std::string("Scene configuration adoption is invalid: ") + failed_guard);
  }
  for (std::size_t index = 0U;
       index < request.execution_host_bindings.size(); ++index) {
    if (!request.execution_host_bindings[index].valid() ||
        (index != 0U && !core::CanonicalProjectionNativeBindingLess(
                           request.execution_host_bindings[index - 1U],
                           request.execution_host_bindings[index]))) {
      throw std::invalid_argument("qualified Scene adoption bindings are invalid");
    }
  }
}

template <typename Request>
void RequireHeldAdoptionParent(
    const Request &request,
    core::IKernelCellAdmissionPort *admission,
    const core::IManagerKernelApplyFencePort *original_parent_fence,
    const core::ApplyFenceAcquireAttemptQueryDTO *original_fence_attempt = nullptr) {
  if (admission == nullptr) {
    throw LogicalSceneOwnerConflictError(
        "qualified Scene adoption held-parent dependencies are unavailable");
  }
  const auto &claim = AdoptionClaim(request);
  const auto &held_fence_query = AdoptionFence(request);
  const auto receipt = admission->GetCellAdmissionRecoveryReceipt(
      {claim.intent, claim, std::nullopt});
  if (!receipt.ok() || !AdoptionReceiptValid(request, receipt.value()) ||
      receipt.value().status !=
          core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
      receipt.value().matched_claim != claim ||
      receipt.value().matched_release.has_value()) {
    throw LogicalSceneOwnerConflictError(
        "qualified Scene adoption requires the exact still-held Apply claim");
  }
  if (original_fence_attempt != nullptr) {
    const auto &query = *original_fence_attempt;
    const auto &intent = query.persisted_intent;
    const auto &held = held_fence_query;
    const auto &predecessor = request.expected_predecessor_publication;
    if (original_parent_fence == nullptr || !query.valid() ||
        intent.owner_transaction != held.owner_transaction ||
        intent.scope != held.expected_scope ||
        intent.expected_retained_bundle_id != predecessor.request.candidate_retained_bundle_id ||
        intent.expected_graph_revision != predecessor.request.candidate_graph_revision) {
      throw LogicalSceneOwnerConflictError(
          "qualified Scene staging requires the exact original fence attempt");
    }
    // This read proves held presence without inventing candidate configuration
    // rows. It is not an interlock; only the original Manager operation may
    // continue, after joining its own stored attempt and complete acceptance.
    const auto presence = original_parent_fence->ReconcileAcquireAttempt(query);
    if (!presence.matches(query) ||
        presence.state != core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact ||
        !presence.admission_fenced || !presence.accepted_fence.has_value() ||
        presence.accepted_fence->configuration_fence_id != held.configuration_fence_id ||
        presence.accepted_fence->configuration_fence_generation != held.configuration_fence_generation ||
        presence.accepted_fence->captured_scope != held.expected_scope ||
        presence.execution_host_epoch.publication_generation != predecessor.publication_generation) {
      throw LogicalSceneOwnerConflictError(
          "qualified Scene staging requires the exact still-held Apply fence");
    }
    return;
  }
  // This relies on the original sole Manager producer's live operation and
  // durable exclusion. QueryStatus is not an interlock or a view.
  const auto status = original_parent_fence->QueryStatus(held_fence_query);
  if (!status.matches(held_fence_query) ||
      status.state != core::ConfigurationFenceState::kHeld ||
      !status.admission_fenced) {
    throw LogicalSceneOwnerConflictError(
        "qualified Scene adoption requires the exact still-held Apply fence");
  }
}

template <typename Request>
void RequireVerifiedAdoptionCandidate(
    const Request &request,
    const deployment::IVerifiedRetainedBundleSource *source,
    const core::IPlannerSceneProjectionResolver *resolver) {
  if (source == nullptr || resolver == nullptr) {
    throw LogicalSceneOwnerConflictError(
        "qualified Scene adoption candidate dependencies are unavailable");
  }
  const auto candidate = source->get_verified(
      request.candidate_retained_bundle.retained_bundle_id);
  std::set<std::pair<core::CapabilityProviderBindingId,
                     core::CapabilityProviderBindingRevision>> supplied;
  for (const auto &binding : request.execution_host_bindings) {
    if (!supplied.emplace(binding.provider_binding_id,
                          binding.provider_binding_revision).second) {
      throw LogicalSceneOwnerConflictError(
          "qualified Scene adoption repeats a required provider binding");
    }
  }
  if (candidate == nullptr ||
      !deployment::verify_retained_bundle_identity(*candidate) ||
      candidate->value() != request.candidate_retained_bundle ||
      !CandidateBindingsMatch(candidate->value(), request.execution_host_bindings)) {
    throw LogicalSceneOwnerConflictError(
        "qualified Scene adoption candidate or binding set is not exact");
  }
}

template <typename Request>
void RequireAdoptionCurrentScene(
    const DurableState &state,
    const Request &request,
    const bool completed = false) {
  // Identity only: the current Scene may have moved on since the request.
  const auto expected =
      completed ? AdoptionTarget(request, request.expected_snapshot_identity.revision)
                : request.expected_active_projection_revision;
  if (state.projection_desynchronized || state.unresolved_scene_transaction ||
      !state.active_projection.has_value() ||
      !core::SameProjectionIdentity(*state.active_projection, expected) ||
      !state.active_scene_snapshot.has_value() ||
      !SameSnapshotScene(state.active_scene_snapshot->identity(),
                         request.expected_snapshot_identity)) {
    throw LogicalSceneOwnerConflictError(
        "qualified Scene adoption requires the same synchronized current Scene");
  }
}

template <typename Request>
void RequireAdoptionRow(
    const MaintenanceInitializationState &row,
    const Request &request) {
  if (!core::SameProjectionIdentity(row.predecessor, request.expected_active_projection_revision) ||
      !core::SameProjectionIdentity(
          row.target, AdoptionTarget(request, request.expected_snapshot_identity.revision)) ||
      !SameSnapshotScene(row.authoritative_snapshot.identity(), request.expected_snapshot_identity) ||
      row.bindings != request.execution_host_bindings ||
      row.initial_baseline_request_digest.has_value() ||
      row.repaired_transaction_sequence.has_value()) {
    throw LogicalSceneOwnerConflictError(
        "qualified Scene adoption identity is already bound differently");
  }
}

void RequireCurrentPublication(
    const core::IActiveBundlePublicationReader &reader,
    const ActiveBundlePublicationReceiptDTO &expected) {
  const auto current = reader.ReadActiveBundlePublication(expected.publication_generation);
  if (!current.ok() || current.value() != expected) {
    throw LogicalSceneOwnerConflictError(
        "qualified Scene adoption publication is not the exact current head");
  }
}

// Returns the index of the oldest of `rows` no reader still needs, given how
// many completions (in order) are already folded into the Scene history
// checkpoint. See MakeRoomForMaintenanceInitialization.
[[nodiscard]] std::optional<std::size_t> OldestEvictableMaintenanceInitialization(
    const std::vector<const MaintenanceInitializationState *> &rows,
    const std::size_t folded_completions) {
  std::optional<std::size_t> latest_completed;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    if (rows[index]->publication_receipt.has_value()) {
      latest_completed = index;
    }
  }
  std::size_t completion_ordinal = 0U;
  for (std::size_t index = 0U; index + 1U < rows.size(); ++index) {
    const auto &row = *rows[index];
    if (row.publication_receipt.has_value()) {
      if (completion_ordinal++ < folded_completions &&
          latest_completed != index) {
        return index;
      }
      continue;
    }
    const bool terminal =
        row.result_state == ProjectionMaintenanceInitializationState::kFailed ||
        row.result_state ==
            ProjectionMaintenanceInitializationState::kInterrupted;
    const bool superseded =
        latest_completed.has_value() && index < *latest_completed;
    if (terminal && (!row.effects_unknown || superseded)) {
      return index;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::vector<const MaintenanceInitializationState *>
MaintenanceRows(const DurableState &state) {
  std::vector<const MaintenanceInitializationState *> rows;
  rows.reserve(state.maintenance_initializations.size());
  for (const auto &row : state.maintenance_initializations) {
    rows.push_back(&row);
  }
  return rows;
}

} // namespace

std::vector<const MaintenanceInitializationState *>
MaintenanceEvictions(const SceneStoreMemory &memory, const DurableState &state,
                     std::size_t folded, std::uint64_t freed,
                     const std::uint64_t bytes, bool one) {
  // The rule applied again after each eviction, as the count limit applies
  // it once per new initialization.
  auto rows = MaintenanceRows(state);
  std::vector<const MaintenanceInitializationState *> evictions;
  while (one || SceneFileBytes(memory) - freed + bytes >
                    kSceneStoreRecordFileBytes) {
    const auto evictable = OldestEvictableMaintenanceInitialization(rows, folded);
    if (!evictable.has_value()) {
      break;
    }
    const auto *row = rows[*evictable];
    if (const auto found =
            memory.maintenance_file_bytes.find(row->initialization_sequence);
        found != memory.maintenance_file_bytes.end()) {
      freed += found->second;
    }
    if (row->publication_receipt.has_value()) {
      --folded;
    }
    evictions.push_back(row);
    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(*evictable));
    one = false;
  }
  return evictions;
}

// Deletes one initialization and its rows in every child table inside the
// caller's write transaction (the delete guards open only while
// compaction_in_progress is set). A folded completion leaves the checkpoint's
// folded-completion count, which indexes the retained completions.
void DeleteMaintenanceInitialization(SceneStoreMemory &memory,
                                     persistence::SqliteRecord &record,
                                     const MaintenanceInitializationState &row) {
  const auto sequence =
      CheckedSafeCounter(row.initialization_sequence, "initialization sequence");
  record.Statement("UPDATE logical_scene_history_state SET "
                   "compaction_in_progress=1 WHERE singleton=1 AND "
                   "compaction_in_progress=0");
  record.ExpectChanges(1);
  for (const std::string_view table :
       {"logical_scene_initial_baseline_results",
        "logical_scene_initial_baseline_intents",
        "logical_scene_initial_baseline_requests",
        "logical_scene_maintenance_initialization_completions",
        "logical_scene_maintenance_initialization_results",
        "logical_scene_maintenance_initialization_bindings",
        "logical_scene_maintenance_initialization_intents"}) {
    record.Statement("DELETE FROM " + std::string(table) +
                     " WHERE initialization_sequence=?");
    record.Bind(1, sequence);
  }
  record.Statement(row.publication_receipt.has_value()
                       ? "UPDATE logical_scene_history_state SET "
                         "compacted_maintenance_completion_count="
                         "compacted_maintenance_completion_count-1,"
                         "compaction_in_progress=0 WHERE singleton=1"
                       : "UPDATE logical_scene_history_state SET "
                         "compaction_in_progress=0 WHERE singleton=1");
  std::erase_if(memory.maintenance_initializations,
                [&](const MaintenanceInitializationState &value) {
                  return value.initialization_sequence ==
                         row.initialization_sequence;
                });
  if (row.publication_receipt.has_value()) {
    --memory.history.compacted_maintenance_completion_count;
  }
  memory.maintenance_file_bytes.erase(row.initialization_sequence);
}

namespace {

// Bound the maintenance-initialization history by eviction instead of
// refusal. When it is full, or the new initialization's rows would not fit
// the store's file bytes (od212), delete, inside the caller's write
// transaction and before its insert, the oldest initialization no reader can
// still need:
//  - not the latest: AccessStatus, health, the Begin/Install/Complete
//    continuation and the Manager's in-flight Apply read it;
//  - not the latest completed one: ActiveExecutionHostBindings, the durable
//    head's Deployment identity and legacy-uncertainty repair read it;
//  - not one that may still progress: no result yet, or initialized without
//    completion (InstallInitialScene and CompleteMaintenanceInitialization
//    find any row by Apply identity);
//  - not an interrupted (effects-unknown) one that no later completion has
//    superseded with a fresh initialization (the workbench classifier reports
//    it as unresolved);
//  - not a completion the Scene replay still consumes: replay applies the
//    completions in order after the history checkpoint, skipping the first
//    `compacted_maintenance_completion_count`, so only those folded ones are
//    evictable and evicting one decrements that count.
// A failed row and a superseded interrupted one are terminal and never read
// again; exact status replay of an evicted Apply returns nothing, like an
// evicted Manager Apply. When rows are needed only because completions are not
// yet folded, fold them first with the transparent history compaction at the
// settled head, settling held authored-Scene claims first as the commit path
// does. For the file bytes, the resident Scene transactions are folded first
// (a fold loses nothing), then initializations are evicted one after the
// other until the rows fit. Returns nullopt when every retained
// initialization is still needed, for the count or for the bytes; nothing
// has changed then.
// The file bytes one more initialization can take, error texts aside
// (od212): its intent with `snapshot_bytes` of Scene, its `bindings`, its
// result and completion, and with `baseline` its Program baseline request,
// intent (the largest Scene) and result, every identifier at its bound.
[[nodiscard]] std::uint64_t
MaintenanceInitializationFileBytesBound(const std::size_t snapshot_bytes,
                                        const std::size_t bindings,
                                        const bool baseline) {
  constexpr std::size_t kIdentifier = kMaximumSceneStoreIdentifierBytes;
  const auto sized = [](const std::size_t integers, const std::size_t ids,
                        const std::size_t nulls, const std::size_t text) {
    RecordSize size;
    for (std::size_t index = 0U; index < integers; ++index) {
      size.Integer(std::numeric_limits<std::int64_t>::max());
    }
    for (std::size_t index = 0U; index < ids; ++index) {
      size.Text(kIdentifier);
    }
    for (std::size_t index = 0U; index < nulls; ++index) {
      size.Null();
    }
    if (text != 0U) {
      size.Text(text);
    }
    return size.bytes();
  };
  auto bytes =
      SceneRowFileBytes(
          sized(7U, 8U, 0U, snapshot_bytes),
          SceneTable("logical_scene_maintenance_initialization_intents")) +
      bindings * SceneRowFileBytes(
                     sized(2U, 4U, 0U, 0U),
                     SceneTable("logical_scene_maintenance_initialization_bindings")) +
      SceneRowFileBytes(
          sized(2U, 1U, 2U, 0U),
          SceneTable("logical_scene_maintenance_initialization_results")) +
      SceneRowFileBytes(
          sized(3U, 2U, 0U, 0U),
          SceneTable("logical_scene_maintenance_initialization_completions"));
  if (baseline) {
    bytes += SceneRowFileBytes(sized(1U, 1U, 0U, 0U),
                               SceneTable("logical_scene_initial_baseline_requests")) +
             SceneRowFileBytes(sized(2U, 4U, 0U, kMaximumLogicalSceneTransactionBytes),
                               SceneTable("logical_scene_initial_baseline_intents")) +
             SceneRowFileBytes(sized(1U, 1U, 2U, 0U),
                               SceneTable("logical_scene_initial_baseline_results"));
  }
  return bytes;
}

struct MaintenanceRoom final {
  std::vector<const MaintenanceInitializationState *> evictions;
  std::optional<PreparedSceneHistoryCompaction> compaction;
  std::optional<SceneTransactionStoreState> transactions;
};

[[nodiscard]] std::optional<MaintenanceRoom>
MakeRoomForMaintenanceInitialization(
    const SceneStoreMemory &memory,
    core::IKernelCellAdmissionPort *cell_admission,
    const DurableState &state, const std::uint64_t bytes) {
  const bool full = state.maintenance_initializations.size() >=
                    kMaximumMaintenanceInitializations;
  const auto fits = [&](const std::uint64_t freed) {
    return SceneFileBytes(memory) - freed + bytes <= kSceneStoreRecordFileBytes;
  };
  if (!full && fits(0U)) {
    return MaintenanceRoom{};
  }
  const auto rows = MaintenanceRows(state);
  auto folded = memory.history.compacted_maintenance_completion_count;
  const bool fold_for_count =
      full && !OldestEvictableMaintenanceInitialization(rows, folded) &&
      OldestEvictableMaintenanceInitialization(
          rows, kMaximumMaintenanceInitializations);
  if (full && !fold_for_count &&
      !OldestEvictableMaintenanceInitialization(rows, folded)) {
    return std::nullopt;
  }
  MaintenanceRoom room;
  std::uint64_t freed = 0U;
  if (fold_for_count || (!fits(0U) && memory.transaction_file_bytes != 0U)) {
    auto transactions = ReadSceneTransactionStoreState(memory, state);
    bool settled = !state.projection_desynchronized &&
                   !state.unresolved_scene_transaction &&
                   !transactions.projection_desynchronized &&
                   !transactions.unresolved_native_attempt;
    if (settled && cell_admission != nullptr &&
        transactions.resident_transaction_count != 0U) {
      try {
        SettleRecoveredAuthoredSceneClaims(memory, *cell_admission);
      } catch (const CellAdmissionTemporarilyUnavailable &) {
        settled = false;
      }
    }
    if (settled) {
      room.compaction =
          PrepareSceneHistoryCompaction(memory, state, transactions, true);
      room.transactions = std::move(transactions);
      folded = room.compaction->history.compacted_maintenance_completion_count;
      freed = memory.transaction_file_bytes;
    } else if (fold_for_count) {
      return std::nullopt;
    }
  }
  room.evictions =
      MaintenanceEvictions(memory, state, folded, freed, bytes, full);
  for (const auto *row : room.evictions) {
    freed += memory.maintenance_file_bytes.contains(row->initialization_sequence)
                 ? memory.maintenance_file_bytes.at(row->initialization_sequence)
                 : 0U;
  }
  if ((full && room.evictions.empty()) || !fits(freed)) {
    return std::nullopt;
  }
  return room;
}

// Applies the fold and the evictions to memory, ahead of the new row's
// statements in the same record.
void ApplyMaintenanceRoom(SceneStoreMemory &memory,
                          persistence::SqliteRecord &record,
                          MaintenanceRoom room, const DurableState &state) {
  if (room.compaction.has_value()) {
    CompactSceneTransactionHistory(memory, record, std::move(*room.compaction),
                                   state, *room.transactions);
  }
  for (const auto *row : room.evictions) {
    DeleteMaintenanceInitialization(memory, record, *row);
  }
}

[[nodiscard]] std::uint64_t
NextMaintenanceInitializationSequence(const DurableState &state) {
  const auto *latest = state.LatestMaintenanceInitialization();
  return latest == nullptr ? 1U : latest->initialization_sequence + 1U;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene {

std::optional<ProjectionMaintenanceInitializationStatusDTO>
LogicalSceneOwner::ReadMaintenanceInitialization(
    const ProjectionMaintenanceInitializationQueryDTO &query) const {
  if (!query.valid()) {
    throw std::invalid_argument(
        "maintenance initialization query is invalid");
  }
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadStateTransaction();
  const auto found = std::find_if(
      state.maintenance_initializations.begin(),
      state.maintenance_initializations.end(), [&](const auto &value) {
        return value.apply_id == query.apply_id &&
               value.apply_generation == query.apply_generation;
      });
  if (found == state.maintenance_initializations.end()) {
    return std::nullopt;
  }
  return MaintenanceStatus(*found);
}

std::vector<core::ProjectionHealthDTO>
LogicalSceneOwner::StageOriginalManagerConfigurationAdoption(
    const QualifiedSceneConfigurationAdoptionRequest &request,
    const core::IManagerKernelApplyFencePort &original_parent_fence,
    const core::ApplyFenceAcquireAttemptQueryDTO &original_fence_attempt) {
  return OriginalManagerAdoptionProjectionHealthImpl(
      request, original_parent_fence, original_fence_attempt, true);
}

std::vector<core::ProjectionHealthDTO>
LogicalSceneOwner::ReadOriginalManagerConfigurationAdoptionProjectionHealth(
    const QualifiedSceneConfigurationAdoptionRequest &request,
    const core::IManagerKernelApplyFencePort &original_parent_fence,
    const core::ApplyFenceAcquireAttemptQueryDTO &original_fence_attempt) {
  return OriginalManagerAdoptionProjectionHealthImpl(
      request, original_parent_fence, original_fence_attempt, false);
}

template <typename Request>
std::vector<core::ProjectionHealthDTO>
LogicalSceneOwner::OriginalManagerAdoptionProjectionHealthImpl(
    const Request &request,
    const core::IManagerKernelApplyFencePort &original_parent_fence,
    const core::ApplyFenceAcquireAttemptQueryDTO &original_fence_attempt,
    const bool stage) {
  ValidateAdoptionRequest(request, impl_->fixed_query_.cell_id);
  std::lock_guard operation_lock(impl_->maintenance_operation_mutex_);
  RequireVerifiedAdoptionCandidate(request, impl_->retained_bundle_source_,
                                  impl_->projection_resolver_);
  RequireHeldAdoptionParent(request, impl_->cell_admission_, &original_parent_fence,
                            &original_fence_attempt);
  RequireCurrentPublication(impl_->publication_reader_, request.expected_predecessor_publication);
  std::unique_lock lock(impl_->mutex_);
  const auto state = impl_->ReadStateTransaction();
  RequireAdoptionCurrentScene(state, request);
  if (impl_->live_adoption_.has_value() || !AccessStatus(state).scene_writes_open) {
    throw LogicalSceneOwnerConflictError(
        "candidate projection staging has no open original Scene window");
  }
  // Stage from the latest Scene. A later commit does not invalidate staging:
  // Begin brings the candidate projections forward to the Scene it captures.
  const auto snapshot = std::make_shared<const LogicalSceneSnapshotDTO>(*state.active_scene_snapshot);
  const auto target = AdoptionTarget(request, snapshot->revision);
  lock.unlock();

  // Registry preparation already owns each uninitialized candidate model and
  // its exact predecessor. Rebuild uses that same owner/job; it does not select
  // a public planner, change the logical Scene or retire a surviving projection.
  std::vector<core::ProjectionHealthDTO> health;
  health.reserve(request.execution_host_bindings.size());
  for (const auto &binding : request.execution_host_bindings) {
    auto projection = ResolveRequiredProjection(
            binding, impl_->projection_resolver_, target);
    auto observed = projection.preflight_health;
    if (observed.execution_host_binding != binding ||
        (observed.state == ProjectionHealthState::kHealthy &&
         !observed.healthy_for_identity(target))) {
      throw LogicalSceneOwnerConflictError(
          "candidate projection staging requires its exact binding and target");
    }
    if (stage && !observed.healthy_for_identity(target)) {
      const core::NativeProjectionRebuildDTO rebuild{
          binding, target,
          request.candidate_retained_bundle.compiled_products.planning_collision_model,
          snapshot, std::nullopt, std::nullopt};
      const auto rebuilt = projection.projection->RebuildProjection(rebuild);
      const bool accepted = rebuilt.result == core::ProjectionCommitResultStateDTO::kAccepted &&
                            !rebuilt.acknowledged_projection_revision.has_value() &&
                            rebuilt.resulting_health.state == ProjectionHealthState::kRebuilding;
      const bool committed = rebuilt.result == core::ProjectionCommitResultStateDTO::kCommitted &&
                             rebuilt.acknowledged_projection_revision == target &&
                             rebuilt.resulting_health.healthy_for_target(target);
      if (!rebuilt.presence_valid() || rebuilt.transaction_sequence.has_value() ||
          rebuilt.proposed_projection_revision != target ||
          rebuilt.resulting_health.execution_host_binding != binding ||
          (!accepted && !committed)) {
        const auto detail = rebuilt.error.has_value() ? ": " + rebuilt.error->message : std::string{};
        throw LogicalSceneOwnerConflictError(
            "candidate native projection did not accept the exact preserved snapshot" + detail);
      }
      observed = rebuilt.resulting_health;
    }
    health.push_back(std::move(observed));
  }

  // These remote observations detect uncertainty, never roll back native work.
  // Manager retains its original operation/claim/fence and candidate owners on
  // failure. The existing published-head route remains the ordinary selector.
  RequireHeldAdoptionParent(request, impl_->cell_admission_, &original_parent_fence,
                            &original_fence_attempt);
  RequireCurrentPublication(impl_->publication_reader_, request.expected_predecessor_publication);
  lock.lock();
  const auto current = impl_->ReadStateTransaction();
  RequireAdoptionCurrentScene(current, request);
  if (!AccessStatus(current).scene_writes_open) {
    throw LogicalSceneOwnerConflictError(
        "logical Scene closed while staging candidate projections");
  }
  return health;
}

ProjectionMaintenanceInitializationStatusDTO
LogicalSceneOwner::BeginOriginalManagerConfigurationAdoption(
    const QualifiedSceneConfigurationAdoptionRequest &request,
    const core::IManagerKernelApplyFencePort &original_parent_fence) {
  return BeginQualifiedAdoptionImpl(request, &original_parent_fence);
}

template <typename Request>
ProjectionMaintenanceInitializationStatusDTO
LogicalSceneOwner::BeginQualifiedAdoptionImpl(
    const Request &request,
    const core::IManagerKernelApplyFencePort *original_parent_fence) {
  ValidateAdoptionRequest(request, impl_->fixed_query_.cell_id);
  std::lock_guard operation_lock(impl_->maintenance_operation_mutex_);
  RequireVerifiedAdoptionCandidate(request, impl_->retained_bundle_source_,
                                  impl_->projection_resolver_);
  RequireHeldAdoptionParent(request, impl_->cell_admission_, original_parent_fence);
  RequireCurrentPublication(impl_->publication_reader_,
                            request.expected_predecessor_publication);

  std::unique_lock lock(impl_->mutex_);
  auto state = impl_->ReadStateTransaction();
  RequireAdoptionCurrentScene(state, request);
  const auto &owner = AdoptionOwner(request);
  const auto find_operation = [&](const DurableState &value) {
    return std::find_if(value.maintenance_initializations.begin(),
                        value.maintenance_initializations.end(),
                        [&](const auto &row) {
                          return row.apply_id == owner.apply_id &&
                                 row.apply_generation == owner.apply_generation;
                        });
  };
  auto found = find_operation(state);
  if (found != state.maintenance_initializations.end()) {
    RequireAdoptionRow(*found, request);
    if (!LiveAdoptionMatches(impl_->live_adoption_, request) ||
        found != std::prev(state.maintenance_initializations.end()) ||
        found->result_state != ProjectionMaintenanceInitializationState::kInitialized ||
        found->publication_receipt.has_value()) {
      throw LogicalSceneOwnerConflictError(
          "qualified Scene adoption has no exact original live continuation");
    }
  } else if (impl_->live_adoption_.has_value() ||
             !AccessStatus(state).scene_writes_open) {
    throw LogicalSceneOwnerConflictError(
        "qualified Scene adoption cannot enter this maintenance window");
  }
  const auto staged_target = AdoptionTarget(request, state.active_scene_snapshot->revision);
  lock.unlock();
  for (const auto &binding : request.execution_host_bindings) {
    const auto projection = ResolveRequiredProjection(
            binding, impl_->projection_resolver_, staged_target);
    if (!projection.preflight_health.healthy_for_identity(staged_target)) {
      throw LogicalSceneOwnerConflictError(
          "qualified Scene adoption requires already healthy candidate projections");
    }
  }
  // The original Manager caller retains durable exclusion, not a frozen
  // host/child view.
  RequireHeldAdoptionParent(request, impl_->cell_admission_, original_parent_fence);
  RequireCurrentPublication(impl_->publication_reader_,
                            request.expected_predecessor_publication);
  lock.lock();
  impl_->InvalidateSceneCommitState();
  state = impl_->ReadStateTransaction();
  RequireAdoptionCurrentScene(state, request);
  found = find_operation(state);
  if (found != state.maintenance_initializations.end()) {
    RequireAdoptionRow(*found, request);
    return MaintenanceStatus(*found);
  }
  if (!AccessStatus(state).scene_writes_open) {
    throw LogicalSceneOwnerConflictError(
        "qualified Scene adoption lost its maintenance window");
  }
  // Capture the latest Scene. Writes close with this row, so it stays the
  // adopted Scene until completion.
  const auto snapshot = *state.active_scene_snapshot;
  const auto predecessor = *state.active_projection;
  const auto target = AdoptionTarget(request, snapshot.revision);

  // Capture the original request before the recorded boundary. Even a lost
  // response cannot let an ordinary call complete this private row. A
  // refusal before the row is recorded releases the capture.
  impl_->live_adoption_ = request;
  try {
    auto &memory = impl_->memory_;
    const auto snapshot_json =
        logical_scene_codec_internal::EncodeSnapshot(snapshot);
    auto room = MakeRoomForMaintenanceInitialization(
        memory, impl_->cell_admission_, state,
        MaintenanceInitializationFileBytesBound(
            snapshot_json.size(), request.execution_host_bindings.size(),
            false));
    if (!room.has_value()) {
      throw LogicalSceneOwnerConflictError(
          "qualified Scene adoption cannot enter this maintenance window");
    }
    const auto sequence = NextMaintenanceInitializationSequence(state);
    static_cast<void>(CheckedSafeCounter(sequence, "initialization sequence"));
    const auto apply_generation = CheckedSafeCounter(
        owner.apply_generation.value(), "Apply generation");
    auto row = NewMaintenanceInitialization(
        sequence, owner.apply_id, owner.apply_generation, predecessor, target,
        snapshot_json, request.execution_host_bindings, std::nullopt,
        std::nullopt);
    persistence::SqliteRecord record;
    ApplyMaintenanceRoom(memory, record, std::move(*room), state);
    InsertMaintenanceIntent(memory, record, sequence, owner.apply_id,
                            apply_generation,
                            predecessor, target, snapshot_json,
                            request.execution_host_bindings, std::nullopt);
    // No Program baseline row: the exact authoritative Scene is already final.
    InsertMaintenanceResult(memory, row, record, sequence,
                            ProjectionMaintenanceInitializationState::kInitialized,
                            std::nullopt, false);
    memory.maintenance_initializations.push_back(std::move(row));
    impl_->Post(std::move(record));
  } catch (...) {
    impl_->live_adoption_.reset();
    throw;
  }
  const auto captured = impl_->ReadStateTransaction();
  const auto result = MaintenanceStatus(*captured.LatestMaintenanceInitialization());
  lock.unlock();
  // Candidate projections staged at an earlier revision of this Scene move to
  // the captured one in place. A failure here is uncertainty, not rollback.
  const auto latest = std::make_shared<const LogicalSceneSnapshotDTO>(snapshot);
  for (const auto &binding : request.execution_host_bindings) {
    const auto projection = ResolveRequiredProjection(
        binding, impl_->projection_resolver_, target);
    const auto &health = projection.preflight_health;
    if (health.healthy_for_target(target)) {
      continue;
    }
    if (!health.healthy_for_identity(target)) {
      throw LogicalSceneOwnerConflictError(
          "qualified Scene adoption lost a healthy candidate projection");
    }
    const auto rebuilt = projection.projection->RebuildProjection(
        {binding, target,
         request.candidate_retained_bundle.compiled_products.planning_collision_model,
         latest, std::pair{owner.apply_id, owner.apply_generation},
         *health.active_projection_revision});
    if (!rebuilt.presence_valid() ||
        rebuilt.result != core::ProjectionCommitResultStateDTO::kCommitted ||
        rebuilt.acknowledged_projection_revision != target ||
        !rebuilt.resulting_health.healthy_for_target(target)) {
      const auto detail = rebuilt.error.has_value() ? ": " + rebuilt.error->message : std::string{};
      throw LogicalSceneOwnerConflictError(
          "candidate native projection did not adopt the captured Scene" + detail);
    }
  }
  if (original_parent_fence != nullptr) {
    // A lost/changed remote response after commit is uncertainty, not rollback.
    // Keep the original continuation and durable row for observation only.
    RequireHeldAdoptionParent(request, impl_->cell_admission_, original_parent_fence);
    RequireCurrentPublication(impl_->publication_reader_, request.expected_predecessor_publication);
  }
  return result;
}

ProjectionMaintenanceInitializationStatusDTO
LogicalSceneOwner::BeginMaintenanceInitialization(
    const ProjectionMaintenanceInitializationRequestDTO &request) {
  if (!request.valid() ||
      request.execution_host_bindings.size() >
          kMaximumNativeProjectionBindings) {
    throw std::invalid_argument(
        "maintenance initialization request is invalid");
  }
  if (impl_->retained_bundle_source_ == nullptr ||
      impl_->projection_resolver_ == nullptr) {
    throw LogicalSceneOwnerConflictError(
        "maintenance initialization dependencies are unavailable");
  }
  const auto initial_scene_digest = InitialSceneRequestDigest(request);
  std::lock_guard operation_lock(impl_->maintenance_operation_mutex_);
  auto candidate = impl_->retained_bundle_source_->get_verified(
      request.candidate_retained_bundle.retained_bundle_id);
  if (candidate == nullptr ||
      !deployment::verify_retained_bundle_identity(*candidate) ||
      candidate->value() != request.candidate_retained_bundle ||
      candidate->value().cell_graph.cell_id != impl_->fixed_query_.cell_id ||
      !CandidateBindingsMatch(candidate->value(),
                              request.execution_host_bindings)) {
    throw LogicalSceneOwnerConflictError(
        "maintenance initialization candidate or binding set is not exact");
  }

  std::unique_lock lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto state = impl_->ReadStateTransaction();
  auto found = std::find_if(
      state.maintenance_initializations.begin(),
      state.maintenance_initializations.end(), [&](const auto &value) {
        return value.apply_id == request.apply_id &&
               value.apply_generation == request.apply_generation;
      });
  bool inserted_intent = false;
  if (found == state.maintenance_initializations.end()) {
    const auto access = AccessStatus(state);
    const auto *latest = state.LatestMaintenanceInitialization();
    const bool prior_settled =
        latest == nullptr || latest->result_state.has_value() ||
        latest->publication_receipt.has_value();
    const bool explicitly_repairable =
        state.projection_desynchronized || state.unresolved_scene_transaction;
    if (!state.active_projection.has_value() ||
        !state.active_scene_snapshot.has_value() ||
        *state.active_projection !=
            request.expected_active_projection_revision ||
        (!access.scene_writes_open && latest == nullptr &&
         !explicitly_repairable) ||
        !prior_settled) {
      throw LogicalSceneOwnerConflictError(
          "maintenance initialization does not match the exact active Scene");
    }
    auto provisional_snapshot = *state.active_scene_snapshot;
    if (provisional_snapshot.revision.revision_id >= kMaximumSafeIntegral ||
        provisional_snapshot.topology_epoch >= kMaximumSafeIntegral) {
      throw LogicalSceneOwnerLimitError(
          "candidate Scene revision cannot advance");
    }
    provisional_snapshot.objects.clear();
    provisional_snapshot.carried_object_attachments.clear();
    ++provisional_snapshot.revision.revision_id;
    ++provisional_snapshot.topology_epoch;
    provisional_snapshot.revision.content_hash =
        core::logical_scene_identity::ComputeLogicalSceneContentHash(
            provisional_snapshot);
    if (!provisional_snapshot.locally_valid()) {
      throw LogicalSceneOwnerIntegrityError(
          "private empty candidate Scene is invalid");
    }
    const ProjectionRevisionDTO target{
        request.candidate_retained_bundle.retained_bundle_id,
        request.candidate_retained_bundle.cell_graph.graph_revision,
        provisional_snapshot.revision,
    };
    auto &memory = impl_->memory_;
    const auto snapshot_json =
        logical_scene_codec_internal::EncodeSnapshot(provisional_snapshot);
    auto room = MakeRoomForMaintenanceInitialization(
        memory, impl_->cell_admission_, state,
        MaintenanceInitializationFileBytesBound(
            snapshot_json.size(), request.execution_host_bindings.size(),
            true));
    if (!room.has_value()) {
      throw LogicalSceneOwnerConflictError(
          "maintenance initialization does not match the exact active Scene");
    }
    const auto sequence = NextMaintenanceInitializationSequence(state);
    const auto apply_generation = CheckedSafeCounter(
        request.apply_generation.value(), "Apply generation");
    std::optional<std::uint64_t> repaired_transaction_sequence;
    if (state.projection_desynchronized &&
        state.latest_uncertain_transaction_sequence.has_value()) {
      static_cast<void>(
          CheckedSafeCounter(*state.latest_uncertain_transaction_sequence,
                             "uncertain transaction sequence"));
      repaired_transaction_sequence =
          state.latest_uncertain_transaction_sequence;
    }
    auto row = NewMaintenanceInitialization(
        sequence, request.apply_id, request.apply_generation,
        request.expected_active_projection_revision, target, snapshot_json,
        request.execution_host_bindings, repaired_transaction_sequence,
        initial_scene_digest);
    persistence::SqliteRecord record;
    ApplyMaintenanceRoom(memory, record, std::move(*room), state);
    InsertMaintenanceIntent(memory, record, sequence, request.apply_id,
                            apply_generation,
                            request.expected_active_projection_revision,
                            target, snapshot_json,
                            request.execution_host_bindings,
                            repaired_transaction_sequence);
    SizedRow baseline_request(
        record,
        "INSERT INTO logical_scene_initial_baseline_requests("
        "initialization_sequence,request_digest) VALUES(?,?)");
    baseline_request.Bind(1, static_cast<std::int64_t>(sequence));
    baseline_request.Bind(2, initial_scene_digest.value());
    memory.maintenance_file_bytes[sequence] += SceneRowFileBytes(
        baseline_request.record_bytes(),
        SceneTable("logical_scene_initial_baseline_requests"));
    memory.maintenance_initializations.push_back(std::move(row));
    impl_->Post(std::move(record));
    inserted_intent = true;
    state = impl_->ReadStateTransaction();
    found = std::prev(state.maintenance_initializations.end());
  }

  if (found->predecessor != request.expected_active_projection_revision ||
      found->target.retained_bundle_id !=
          request.candidate_retained_bundle.retained_bundle_id ||
      found->target.graph_revision !=
          request.candidate_retained_bundle.cell_graph.graph_revision ||
      found->bindings != request.execution_host_bindings ||
      found->initial_baseline_request_digest != initial_scene_digest) {
    throw LogicalSceneOwnerConflictError(
        "maintenance initialization identity is already bound differently");
  }
  if (found->result_state.has_value() ||
      found->publication_receipt.has_value()) {
    return MaintenanceStatus(*found);
  }

  // Persist and validate the exact operation while holding the Scene state
  // lock, then release that lock before invoking native projections. The
  // dedicated operation lock above prevents duplicate effects while ordinary
  // status and health reads remain responsive during heavy initialization.
  const auto initialization = *found;
  lock.unlock();

  bool progressing = false;
  const LogicalSceneSnapshotRoot snapshot(
      std::make_shared<const LogicalSceneSnapshotDTO>(
          initialization.authoritative_snapshot));
  std::optional<ErrorDTO> failure;
  bool effects_unknown = false;
  bool projection_effect_pending_or_committed = false;
  for (const auto &binding : initialization.bindings) {
    try {
      auto projection = ResolveRequiredProjection(
          binding, impl_->projection_resolver_, initialization.target);
      if (projection.preflight_health.healthy_for_target(
              initialization.target)) {
        // A previous poll may already have installed this projection. Its
        // effect still counts if another participant subsequently refuses.
        projection_effect_pending_or_committed = true;
        continue;
      }
      const bool same_retained_product =
          initialization.predecessor.retained_bundle_id ==
              initialization.target.retained_bundle_id &&
          initialization.predecessor.graph_revision ==
              initialization.target.graph_revision &&
          initialization.predecessor.scene_revision.scene_id ==
              initialization.target.scene_revision.scene_id &&
          initialization.target.scene_revision.revision_id >
              initialization.predecessor.scene_revision.revision_id;
      const bool exact_apply_predecessor =
          same_retained_product &&
          projection.preflight_health.healthy_for_target(
              initialization.predecessor);
      if (!inserted_intent) {
        if (projection.preflight_health.state ==
                ProjectionHealthState::kRebuilding ||
            projection.preflight_health.state ==
                ProjectionHealthState::kUninitialized) {
          progressing = true;
          projection_effect_pending_or_committed =
              projection_effect_pending_or_committed ||
              projection.preflight_health.state ==
                  ProjectionHealthState::kRebuilding;
          continue;
        }
        if (!exact_apply_predecessor) {
          failure = ErrorDTO{
              "native_projection_initialization_failed",
              "a required native projection is neither at the Apply target "
              "nor at its exact predecessor",
              {}};
          effects_unknown =
              projection_effect_pending_or_committed ||
              projection.preflight_health.state ==
                  ProjectionHealthState::kDesynchronized ||
              projection.preflight_health.state ==
                  ProjectionHealthState::kUnavailable;
          break;
        }
      }
      const core::NativeProjectionRebuildDTO rebuild{
          binding,
          initialization.target,
          candidate->value().compiled_products.planning_collision_model,
          snapshot.shared(),
          exact_apply_predecessor
              ? std::optional<std::pair<ApplyId, ApplyGeneration>>{
                    std::pair{initialization.apply_id,
                              initialization.apply_generation}}
              : std::nullopt,
          exact_apply_predecessor
              ? std::optional<ProjectionRevisionDTO>{
                    initialization.predecessor}
              : std::nullopt,
      };
      const auto rebuilt = projection.projection->RebuildProjection(rebuild);
      if (rebuilt.result == core::ProjectionCommitResultStateDTO::kAccepted &&
          rebuilt.presence_valid() && !rebuilt.transaction_sequence.has_value() &&
          rebuilt.proposed_projection_revision == initialization.target &&
          rebuilt.resulting_health.execution_host_binding == binding) {
        progressing = true;
        projection_effect_pending_or_committed = true;
        continue;
      }
      if (!rebuilt.presence_valid() ||
          rebuilt.result != core::ProjectionCommitResultStateDTO::kCommitted ||
          rebuilt.acknowledged_projection_revision != initialization.target ||
          !rebuilt.resulting_health.healthy_for_target(
              initialization.target)) {
        const std::string detail =
            rebuilt.error.has_value()
                ? ": " + rebuilt.error->code + ": " +
                      rebuilt.error->message
                : std::string{};
        failure = ErrorDTO{
            "native_projection_initialization_failed",
            "a required native projection did not acknowledge the exact "
            "authoritative snapshot" + detail,
            {}};
        effects_unknown =
            projection_effect_pending_or_committed ||
            !rebuilt.rollback_proven;
        break;
      }
      projection_effect_pending_or_committed = true;
    } catch (const std::exception &error) {
      failure = ErrorDTO{"native_projection_initialization_uncertain",
                         error.what(), {}};
      effects_unknown = true;
      break;
    } catch (...) {
      failure = ErrorDTO{"native_projection_initialization_uncertain",
                         "native projection initialization raised a "
                         "non-standard exception",
                         {}};
      effects_unknown = true;
      break;
    }
  }

  if (progressing && !failure.has_value()) {
    return MaintenanceStatus(
        initialization,
        ProjectionMaintenanceInitializationState::kInitializing);
  }
  lock.lock();
  persistence::SqliteRecord result;
  InsertMaintenanceResult(
      impl_->memory_,
      MaintenanceRow(impl_->memory_, initialization.initialization_sequence),
      result, initialization.initialization_sequence,
      failure.has_value()
          ? (effects_unknown
                 ? ProjectionMaintenanceInitializationState::kInterrupted
                 : ProjectionMaintenanceInitializationState::kFailed)
          : ProjectionMaintenanceInitializationState::kInitialized,
      failure, effects_unknown);
  impl_->Post(std::move(result));
  const auto completed = impl_->ReadStateTransaction();
  return MaintenanceStatus(*completed.LatestMaintenanceInitialization());
}

ProjectionMaintenanceInitializationStatusDTO
LogicalSceneOwner::InstallInitialScene(
    const ProjectionMaintenanceInitializationRequestDTO &request) {
  if (!request.valid() ||
      impl_->projection_resolver_ == nullptr) {
    throw std::invalid_argument("initial Scene installation request is invalid");
  }
  const auto request_digest = InitialSceneRequestDigest(request);
  std::lock_guard operation_lock(impl_->maintenance_operation_mutex_);
  std::unique_lock lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto state = impl_->ReadStateTransaction();
  const auto find_initialization = [&](const DurableState &value) {
    return std::find_if(
        value.maintenance_initializations.begin(),
        value.maintenance_initializations.end(), [&](const auto &row) {
          return row.apply_id == request.apply_id &&
                 row.apply_generation == request.apply_generation;
        });
  };
  auto found = find_initialization(state);
  if (found == state.maintenance_initializations.end() ||
      found->predecessor != request.expected_active_projection_revision ||
      found->target.retained_bundle_id !=
          request.candidate_retained_bundle.retained_bundle_id ||
      found->target.graph_revision !=
          request.candidate_retained_bundle.cell_graph.graph_revision ||
      found->bindings != request.execution_host_bindings ||
      found->initial_baseline_request_digest != request_digest) {
    throw LogicalSceneOwnerConflictError(
        "initial Scene request differs from its durable Apply intent");
  }
  if (found->initial_baseline_installed || found->initial_baseline_error ||
      found->publication_receipt.has_value()) {
    return MaintenanceStatus(*found);
  }
  if (found->result_state !=
      ProjectionMaintenanceInitializationState::kInitialized) {
    return MaintenanceStatus(*found);
  }
  // One process-local call may perform the effect. An existing recorded
  // finalization intent without its result is uncertain, never retried.
  if (found->initial_baseline_intent_recorded) {
    throw LogicalSceneOwnerConflictError(
        "initial Scene installation has an unresolved effect");
  }
  const auto provisional = *found;
  lock.unlock();

  std::vector<RequiredSceneProjection> projections;
  std::optional<LogicalSceneSnapshotDTO> final_snapshot;
  std::optional<ErrorDTO> preflight_error;
  try {
    for (const auto &binding : provisional.bindings) {
      auto projection =
          ResolveRequiredProjection(
            binding, impl_->projection_resolver_, provisional.target);
      if (!projection.preflight_health.healthy_for_target(
              provisional.target)) {
        throw LogicalSceneOwnerConflictError(
            "candidate projection is not healthy at its private revision");
      }
      projections.push_back(std::move(projection));
    }
    final_snapshot = BuildInitialSceneBaseline(
        request, provisional.authoritative_snapshot, provisional.target,
        projections);
  } catch (const std::exception &error) {
    preflight_error = ErrorDTO{"initial_scene_preparation_failed",
                                error.what(), {}};
  } catch (...) {
    preflight_error = ErrorDTO{"initial_scene_preparation_failed",
                                "initial Scene preparation aborted", {}};
  }

  lock.lock();
  state = impl_->ReadStateTransaction();
  found = find_initialization(state);
  if (found == state.maintenance_initializations.end() ||
      found->initialization_sequence != provisional.initialization_sequence ||
      found->initial_baseline_request_digest != request_digest ||
      found->initial_baseline_installed || found->initial_baseline_error) {
    throw LogicalSceneOwnerConflictError(
        "initial Scene preparation lost its exact owner intent");
  }
  const auto sequence = static_cast<std::int64_t>(
      provisional.initialization_sequence);
  // A failed or interrupted baseline result dominates the provisional
  // result, as the file replay applies it.
  const auto record_baseline_failure = [](MaintenanceInitializationState &row,
                                          const ErrorDTO &error,
                                          const bool effects_unknown) {
    row.initial_baseline_error = ErrorDTO{error.code, error.message, {}};
    row.initial_baseline_effects_unknown = effects_unknown;
    row.result_state =
        effects_unknown ? ProjectionMaintenanceInitializationState::kInterrupted
                        : ProjectionMaintenanceInitializationState::kFailed;
    row.first_error = row.initial_baseline_error;
    row.effects_unknown = effects_unknown;
  };
  if (preflight_error.has_value()) {
    persistence::SqliteRecord failed;
    SizedRow failed_row(
        failed,
        "INSERT INTO logical_scene_initial_baseline_results("
        "initialization_sequence,state,first_error_code,first_error_message) "
        "VALUES(?,'failed',?,?)");
    failed_row.Bind(1, sequence);
    failed_row.Literal("failed");
    failed_row.Bind(2, preflight_error->code);
    failed_row.Bind(3, preflight_error->message);
    impl_->memory_.maintenance_file_bytes[provisional.initialization_sequence] +=
        SceneRowFileBytes(failed_row.record_bytes(),
                          SceneTable("logical_scene_initial_baseline_results"));
    record_baseline_failure(
        MaintenanceRow(impl_->memory_, provisional.initialization_sequence),
        *preflight_error, false);
    impl_->Post(std::move(failed));
    state = impl_->ReadStateTransaction();
    return MaintenanceStatus(*find_initialization(state));
  }
  const ProjectionRevisionDTO final_target{
      provisional.target.retained_bundle_id,
      provisional.target.graph_revision, final_snapshot->revision};
  const auto final_snapshot_json =
      logical_scene_codec_internal::EncodeSnapshot(*final_snapshot);
  {
    persistence::SqliteRecord accepted;
    SizedRow intent(
        accepted,
        "INSERT INTO logical_scene_initial_baseline_intents("
        "initialization_sequence,target_retained_bundle_id,"
        "target_graph_revision,target_scene_id,target_scene_revision_id,"
        "target_scene_content_hash,snapshot_json) VALUES(?,?,?,?,?,?,?)");
    intent.Bind(1, sequence);
    BindProjection(intent, 2, final_target);
    intent.Bind(7, final_snapshot_json);
    impl_->memory_.maintenance_file_bytes[provisional.initialization_sequence] +=
        SceneRowFileBytes(intent.record_bytes(),
                          SceneTable("logical_scene_initial_baseline_intents"));
    MaintenanceRow(impl_->memory_, provisional.initialization_sequence)
        .initial_baseline_intent_recorded = true;
    impl_->Post(std::move(accepted));
  }
  lock.unlock();

  bool any_committed = false;
  bool uncertain = false;
  std::optional<ErrorDTO> effect_error;
  for (std::size_t index = 0U; index < projections.size(); ++index) {
    const auto &projection = projections[index];
    try {
      const core::NativeProjectionRebuildDTO successor{
          provisional.bindings[index], final_target,
          request.candidate_retained_bundle.compiled_products
              .planning_collision_model,
          std::make_shared<const LogicalSceneSnapshotDTO>(*final_snapshot),
          std::pair{request.apply_id, request.apply_generation},
          provisional.target};
      const auto result =
          projection.projection->RebuildProjection(successor);
      if (result.presence_valid() &&
          result.result == core::ProjectionCommitResultStateDTO::kCommitted &&
          result.acknowledged_projection_revision == final_target &&
          result.resulting_health.healthy_for_target(final_target)) {
        any_committed = true;
        continue;
      }
      uncertain = any_committed || !result.rollback_proven;
      effect_error = ErrorDTO{
          "initial_scene_projection_failed",
          "a required native projection did not acknowledge the final Scene",
          {}};
      break;
    } catch (const std::exception &error) {
      uncertain = true;
      effect_error = ErrorDTO{"initial_scene_projection_uncertain",
                              error.what(), {}};
      break;
    } catch (...) {
      uncertain = true;
      effect_error = ErrorDTO{"initial_scene_projection_uncertain",
                              "native projection effect aborted", {}};
      break;
    }
  }

  lock.lock();
  persistence::SqliteRecord result;
  SizedRow result_row(
      result,
      "INSERT INTO logical_scene_initial_baseline_results("
      "initialization_sequence,state,first_error_code,first_error_message) "
      "VALUES(?,?,?,?)");
  result_row.Bind(1, sequence);
  result_row.Bind(2, std::string_view(effect_error.has_value()
                                          ? (uncertain ? "interrupted" : "failed")
                                          : "installed"));
  if (effect_error.has_value()) {
    result_row.Bind(3, effect_error->code);
    result_row.Bind(4, effect_error->message);
  } else {
    result_row.BindNull(3);
    result_row.BindNull(4);
  }
  impl_->memory_.maintenance_file_bytes[provisional.initialization_sequence] +=
      SceneRowFileBytes(result_row.record_bytes(),
                        SceneTable("logical_scene_initial_baseline_results"));
  auto &row =
      MaintenanceRow(impl_->memory_, provisional.initialization_sequence);
  if (effect_error.has_value()) {
    record_baseline_failure(row, *effect_error, uncertain);
  } else {
    // The installed baseline replaces the provisional target and Scene, as
    // the file returns them.
    auto installed =
        logical_scene_codec_internal::DecodeSnapshot(final_snapshot_json);
    row.target = final_target;
    row.authoritative_snapshot = std::move(installed);
    row.initial_baseline_installed = true;
  }
  impl_->Post(std::move(result));
  state = impl_->ReadStateTransaction();
  return MaintenanceStatus(*find_initialization(state));
}

ProjectionMaintenanceInitializationStatusDTO
LogicalSceneOwner::CompleteMaintenanceInitialization(
    const ProjectionMaintenanceCompletionDTO &completion) {
  return CompleteMaintenanceInitializationImpl(
      completion, static_cast<const QualifiedSceneConfigurationAdoptionRequest *>(nullptr), nullptr);
}

ProjectionMaintenanceInitializationStatusDTO
LogicalSceneOwner::CompleteOriginalManagerConfigurationAdoption(
    const QualifiedSceneConfigurationAdoptionRequest &request,
    const ActiveBundlePublicationReceiptDTO &publication,
    const core::IManagerKernelApplyFencePort &original_parent_fence) {
  ValidateAdoptionRequest(request, impl_->fixed_query_.cell_id);
  if (publication.request.expected_previous_publication_generation !=
      request.expected_predecessor_publication.publication_generation) {
    throw LogicalSceneOwnerConflictError(
        "qualified Scene adoption publication changed its predecessor");
  }
  const auto &owner = AdoptionOwner(request);
  return CompleteMaintenanceInitializationImpl(
      {owner.apply_id, owner.apply_generation,
       AdoptionTarget(request, request.expected_snapshot_identity.revision),
       request.expected_snapshot_identity, publication}, &request, &original_parent_fence);
}

template <typename Request>
ProjectionMaintenanceInitializationStatusDTO
LogicalSceneOwner::CompleteMaintenanceInitializationImpl(
    const ProjectionMaintenanceCompletionDTO &completion,
    const Request *adoption,
    const core::IManagerKernelApplyFencePort *original_parent_fence) {
  if (!completion.valid()) {
    throw std::invalid_argument("maintenance completion is invalid");
  }
  std::lock_guard operation_lock(impl_->maintenance_operation_mutex_);
  if (adoption != nullptr) {
    RequireVerifiedAdoptionCandidate(*adoption, impl_->retained_bundle_source_,
                                    impl_->projection_resolver_);
    RequireHeldAdoptionParent(*adoption, impl_->cell_admission_, original_parent_fence);
  }
  std::unique_lock lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto state = impl_->ReadStateTransaction();
  auto found = std::find_if(
      state.maintenance_initializations.begin(),
      state.maintenance_initializations.end(), [&](const auto &value) {
        return value.apply_id == completion.apply_id &&
               value.apply_generation == completion.apply_generation;
      });
  if (found == state.maintenance_initializations.end() ||
      !core::SameProjectionIdentity(found->target,
                                    completion.target_projection_revision) ||
      !SameSnapshotScene(found->authoritative_snapshot.identity(),
                         completion.authoritative_snapshot_identity)) {
    throw LogicalSceneOwnerConflictError(
        "maintenance completion has no exact initialized Scene operation");
  }
  if (adoption != nullptr) {
    RequireAdoptionRow(*found, *adoption);
    RequireAdoptionCurrentScene(state, *adoption, found->publication_receipt.has_value());
    if ((original_parent_fence != nullptr || !found->publication_receipt.has_value()) &&
        !LiveAdoptionMatches(impl_->live_adoption_, *adoption)) {
      throw LogicalSceneOwnerConflictError(
          "qualified Scene adoption has no exact original live continuation");
    }
  } else if (impl_->live_adoption_.has_value() &&
             AdoptionOwner(*impl_->live_adoption_) ==
                 core::ApplyFenceOwnerDTO{completion.apply_id, completion.apply_generation}) {
    throw LogicalSceneOwnerConflictError(
        "private Scene adoption requires its still-held parent completion");
  }
  if (found->publication_receipt.has_value()) {
    if (!core::SamePublicationIdentity(*found->publication_receipt,
                                       completion.publication_receipt)) {
      throw LogicalSceneOwnerConflictError(
          "maintenance completion replay changed the publication receipt");
    }
    if (adoption == nullptr) {
      return MaintenanceStatus(*found);
    }
  }
  if (found->result_state !=
          ProjectionMaintenanceInitializationState::kInitialized ||
      !found->initial_baseline_installed) {
    throw LogicalSceneOwnerConflictError(
        "maintenance completion requires the final Program baseline");
  }
  const auto initialization = *found;
  lock.unlock();
  const auto current = impl_->publication_reader_.ReadActiveBundlePublication(
      completion.publication_receipt.publication_generation);
  if (!current.ok() || current.value() != completion.publication_receipt) {
    throw LogicalSceneOwnerConflictError(
        "maintenance completion publication is not the exact current head");
  }
  for (const auto &binding : initialization.bindings) {
    auto projection =
        ResolveRequiredProjection(
            binding, impl_->projection_resolver_, initialization.target);
    if (!projection.preflight_health.healthy_for_target(
            initialization.target)) {
      throw LogicalSceneOwnerConflictError(
          "maintenance completion lost exact native initialization health");
    }
  }
  if (adoption != nullptr) {
    RequireHeldAdoptionParent(*adoption, impl_->cell_admission_, original_parent_fence);
    RequireCurrentPublication(impl_->publication_reader_, completion.publication_receipt);
  }

  lock.lock();
  impl_->InvalidateSceneCommitState();
  state = impl_->ReadStateTransaction();
  found = std::find_if(
      state.maintenance_initializations.begin(),
      state.maintenance_initializations.end(), [&](const auto &value) {
        return value.apply_id == completion.apply_id &&
               value.apply_generation == completion.apply_generation;
      });
  if (found == state.maintenance_initializations.end() ||
      found->target != initialization.target ||
      found->authoritative_snapshot.identity() !=
          initialization.authoritative_snapshot.identity() ||
      found->bindings != initialization.bindings) {
    throw LogicalSceneOwnerConflictError(
        "maintenance initialization changed before completion commit");
  }
  if (adoption != nullptr) {
    RequireAdoptionRow(*found, *adoption);
    RequireAdoptionCurrentScene(state, *adoption, found->publication_receipt.has_value());
    if (*state.active_scene_snapshot != initialization.authoritative_snapshot) {
      throw LogicalSceneOwnerConflictError(
          "qualified Scene adoption snapshot changed before completion");
    }
  }
  if (found->publication_receipt.has_value()) {
    if (!core::SamePublicationIdentity(*found->publication_receipt,
                                       completion.publication_receipt)) {
      throw LogicalSceneOwnerConflictError(
          "maintenance completion replay changed the publication receipt");
    }
    const auto result = MaintenanceStatus(*found);
    if (original_parent_fence != nullptr) {
      lock.unlock();
      RequireHeldAdoptionParent(*adoption, impl_->cell_admission_, original_parent_fence);
      RequireCurrentPublication(impl_->publication_reader_, completion.publication_receipt);
      lock.lock();
    }
    if (adoption != nullptr && LiveAdoptionMatches(impl_->live_adoption_, *adoption)) {
      impl_->live_adoption_.reset();
    }
    return result;
  }
  if (found->result_state !=
      ProjectionMaintenanceInitializationState::kInitialized) {
    throw LogicalSceneOwnerConflictError(
        "maintenance initialization is no longer ready for completion");
  }
  persistence::SqliteRecord insert;
  InsertMaintenanceCompletion(impl_->memory_, insert, *found,
                              completion.publication_receipt);
  impl_->Post(std::move(insert));
  const auto completed = impl_->ReadStateTransaction();
  const auto result = MaintenanceStatus(*completed.LatestMaintenanceInitialization());
  if (original_parent_fence != nullptr) {
    // The completion remains durable if this observation is uncertain. The
    // original Manager must retain its exclusions and must not infer success.
    lock.unlock();
    RequireHeldAdoptionParent(*adoption, impl_->cell_admission_, original_parent_fence);
    RequireCurrentPublication(impl_->publication_reader_, completion.publication_receipt);
    lock.lock();
  }
  if (adoption != nullptr) {
    impl_->live_adoption_.reset();
  }
  return result;
}

void LogicalSceneOwner::EndOriginalManagerConfigurationAdoption(
    const core::ApplyId &apply_id, const core::ApplyGeneration apply_generation,
    const std::optional<ActiveBundlePublicationReceiptDTO> &publication) {
  std::lock_guard operation_lock(impl_->maintenance_operation_mutex_);
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadStateTransaction();
  const auto found = std::find_if(
      state.maintenance_initializations.begin(),
      state.maintenance_initializations.end(), [&](const auto &row) {
        return row.apply_id == apply_id &&
               row.apply_generation == apply_generation;
      });
  if (found == state.maintenance_initializations.end()) {
    return;
  }
  const auto release_capture = [&] {
    if (impl_->live_adoption_.has_value() &&
        AdoptionOwner(*impl_->live_adoption_) ==
            core::ApplyFenceOwnerDTO{apply_id, apply_generation}) {
      impl_->live_adoption_.reset();
    }
  };
  if (found->publication_receipt.has_value() ||
      found->result_state !=
          ProjectionMaintenanceInitializationState::kInitialized) {
    release_capture();
    return;
  }
  persistence::SqliteRecord record;
  if (publication.has_value()) {
    RequireCurrentPublication(impl_->publication_reader_, *publication);
    const auto *named = std::get_if<NamedApplyPublicationTransactionDTO>(
        &publication->request.transaction_ref);
    if (named == nullptr || named->apply_id != apply_id ||
        named->apply_generation != apply_generation ||
        !publication->request.expected_previous_publication_generation
             .has_value() ||
        !found->initial_baseline_installed ||
        !core::SameProjectionIdentity(
            publication->request.candidate_projection_revision,
            found->target)) {
      throw LogicalSceneOwnerConflictError(
          "Scene adoption end publication does not name its target");
    }
    impl_->InvalidateSceneCommitState();
    InsertMaintenanceCompletion(impl_->memory_, record, *found, *publication);
  } else {
    record.Statement(
        "UPDATE logical_scene_maintenance_initialization_results SET "
        "state='interrupted',first_error_code=?,first_error_message=?,"
        "effects_unknown=1 WHERE initialization_sequence=? AND "
        "state='initialized'");
    record.Bind(1, kAdoptionEndedWithoutPublicationCode);
    record.Bind(2, kAdoptionEndedWithoutPublicationMessage);
    record.Bind(3, CheckedSafeCounter(found->initialization_sequence,
                                      "initialization sequence"));
    record.ExpectChanges(1);
    impl_->InvalidateSceneCommitState();
    // The result row grows by the error it now carries.
    const auto result_bytes = [&](const std::string_view state,
                                  const bool error) {
      RecordSize size;
      size.Integer(static_cast<std::int64_t>(found->initialization_sequence));
      size.Text(state.size());
      if (error) {
        size.Text(kAdoptionEndedWithoutPublicationCode.size());
        size.Text(kAdoptionEndedWithoutPublicationMessage.size());
      } else {
        size.Null();
        size.Null();
      }
      size.Integer(error ? 1 : 0);
      return SceneRowFileBytes(
          size.bytes(),
          SceneTable("logical_scene_maintenance_initialization_results"));
    };
    auto &file_bytes =
        impl_->memory_.maintenance_file_bytes[found->initialization_sequence];
    file_bytes = file_bytes + result_bytes("interrupted", true) -
                 result_bytes("initialized", false);
    auto &row = MaintenanceRow(impl_->memory_, found->initialization_sequence);
    row.result_state = ProjectionMaintenanceInitializationState::kInterrupted;
    row.first_error =
        ErrorDTO{std::string(kAdoptionEndedWithoutPublicationCode),
                 std::string(kAdoptionEndedWithoutPublicationMessage),
                 {}};
    row.effects_unknown = true;
  }
  impl_->Post(std::move(record));
  release_capture();
}

} // namespace manyforge::scene
