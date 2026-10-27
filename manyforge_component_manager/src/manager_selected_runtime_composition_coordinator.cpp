#include "manyforge_component_manager/manager_selected_runtime_composition_coordinator.hpp"

#include <algorithm>
#include <limits>
#include <manyforge_deployment/content_identity.hpp>
#include <string>
#include <utility>

namespace manyforge::component_manager {
namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

[[noreturn]] void Fail(const std::string &detail) {
  throw ManagerSelectedRuntimeCompositionCoordinatorError(detail);
}

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    Fail(detail);
  }
}

} // namespace

ManagerSelectedRuntimeCompositionCoordinator::
    ManagerSelectedRuntimeCompositionCoordinator(
        SqliteManagerApplyJournal &apply_journal,
        core::IManagerSceneProjectionMaintenancePort &scene_owner,
        IManagerSelectedRuntimeCompositionPort &runtime_composition,
        core::RuntimeEpoch manager_runtime_epoch)
    : apply_journal_(apply_journal), scene_owner_(scene_owner),
      runtime_composition_(runtime_composition),
      manager_runtime_epoch_(std::move(manager_runtime_epoch)) {
  Require(!manager_runtime_epoch_.empty(),
          "selected runtime-composition Manager epoch is empty");
}

core::Result<ManagerApplyComponentBringUpResultDTO>
ManagerSelectedRuntimeCompositionCoordinator::SubmitApplyComponentBringUp(
    const ManagerApplyComponentBringUpRequestDTO &request) {
  std::lock_guard lock(operation_mutex_);
  Require(request.valid() && request.manager_runtime_epoch == manager_runtime_epoch_,
          "Apply child SUBMIT is invalid or belongs to another Manager");
  auto result = runtime_composition_.SubmitApplyComponentBringUp(request);
  if (!result.ok()) return result;
  Require(result.value().valid() && result.value().request == request && result.value().result,
          "managed host returned missing or foreign Apply child SUBMIT evidence");
  return result;
}

core::Result<ManagerApplyComponentBringUpResultDTO>
ManagerSelectedRuntimeCompositionCoordinator::ReadApplyComponentBringUp(
    const ManagerApplyComponentBringUpReadRequestDTO &request) {
  std::lock_guard lock(operation_mutex_);
  Require(request.valid() && request.request.manager_runtime_epoch == manager_runtime_epoch_,
          "Apply child READ is invalid or belongs to another Manager");
  auto result = runtime_composition_.ReadApplyComponentBringUp(request);
  if (!result.ok()) return result;
  Require(result.value().valid() && result.value().request == request.request,
          "managed host returned foreign Apply child READ evidence");
  return result;
}

ManagerSelectedRuntimeCompositionCoordinatorResult
ManagerSelectedRuntimeCompositionCoordinator::PrepareOriginalConfiguration(
    const core::QualifiedSceneConfigurationAdoptionRequestDTO &original,
    const deployment::RetainedBundle &candidate) {
  std::lock_guard lock(operation_mutex_);
  const auto &owner = original.authority.owner;
  const auto status = apply_journal_.ReadStatus(owner.apply_id, owner.apply_generation);
  Require(status && !status->bring_up_plan && !status->terminal_outcome &&
              !status->private_materialization && !status->publication_receipt &&
              status->intent.intent_digest == original.authority.persisted_manager_intent_digest &&
              status->affected_scope && status->fence_attempt && status->fence_acceptance &&
              status->latest_runtime_staged_ready,
          "configuration preparation lacks its exact unpublished original parent");
  const auto &intent = status->intent.intent;
  const auto &claim = original.authority.held_cell_claim;
  const auto &fence_query = original.authority.held_fence_query;
  const auto &accepted = status->fence_acceptance->accepted_fence;
  const auto &attempt = *status->fence_attempt;
  const auto &affected = *status->affected_scope;
  Require(intent.expected_active_projection_revision == original.expected_active_projection_revision &&
              intent.cell_id == apply_journal_.cell_id() &&
              original.expected_snapshot_identity == core::LogicalSceneSnapshotIdentityDTO{
                  intent.expected_active_projection_revision.scene_revision, intent.cell_id} &&
              core::ValidateCellAdmissionRecoveryReceiptQuery({claim.intent, claim, std::nullopt}).ok(),
          "configuration preparation differs from its captured Scene/full claim");
  if (intent.parent_authoring_claim) {
    Require(claim == *intent.parent_authoring_claim,
            "configuration preparation differs from the stored authoring parent");
  } else {
    Require(intent.expected_admission_generation && claim.intent == core::CellAdmissionClaimIntentDTO{
                core::CellAdmissionClaimKindDTO::kManagerApply, core::CellAdmissionOwnerRoleDTO::kManager,
                intent.request_context.request_owner_epoch, intent.apply_id.value(), intent.request_context.request_id,
                status->intent.intent_digest, *intent.expected_admission_generation, {}, std::nullopt, std::nullopt},
            "configuration preparation differs from the stored direct Manager claim");
  }
  Require(deployment::verify_retained_bundle_identity(candidate) &&
              candidate.value() == original.candidate_retained_bundle &&
              candidate.retained_bundle_id() == intent.candidate_retained_bundle_id &&
              candidate.value().cell_graph.graph_revision == intent.candidate_graph_revision &&
              candidate.value().cell_graph.cell_id == intent.cell_id &&
              fence_query.valid() && fence_query.owner_transaction == owner &&
              fence_query.configuration_fence_id == accepted.configuration_fence_id &&
              fence_query.configuration_fence_generation == accepted.configuration_fence_generation &&
              fence_query.expected_scope == accepted.captured_scope &&
              fence_query.manager_runtime_epoch == manager_runtime_epoch_ &&
              accepted.matches(attempt.fence_intent) && attempt.fence_intent.owner_transaction == owner &&
              affected.affected_scope.scope == attempt.fence_intent.scope &&
              affected.affected_scope_digest == attempt.affected_scope_digest,
          "configuration preparation candidate or exact fence differs");
  const auto &ready = status->latest_runtime_staged_ready->staged_ready;
  Require(ready.runtime_units.size() == 1U && original.expected_predecessor_publication.valid() &&
              original.expected_predecessor_publication.publication_generation == ready.predecessor_publication_generation &&
              original.expected_predecessor_publication.durable_public_pointer_content_digest == ready.predecessor_public_pointer_digest &&
              core::SameActiveDeployment(original.expected_predecessor_publication.request.candidate_projection_revision,
                                         intent.expected_active_projection_revision),
          "configuration preparation differs from the actual staged host/head");
  const auto bytes = candidate.canonical_bytes();
  Require(!bytes.empty() && bytes.size() <= static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()),
          "configuration preparation candidate byte extent is invalid");
  const auto &runtime = ready.runtime_units.front();
  const core::ProjectionRevisionDTO target{candidate.retained_bundle_id(),
      candidate.value().cell_graph.graph_revision, original.expected_snapshot_identity.revision};
  const ManagerSelectedRuntimeCompositionRequestDTO request{
      original.expected_snapshot_identity.revision.content_hash, intent.apply_id, intent.apply_generation,
      target, ready.predecessor_publication_generation, ready.predecessor_public_pointer_digest,
      core::ContentDigest(deployment::sha256_digest(bytes)), static_cast<std::uint64_t>(bytes.size()),
      runtime.runtime_unit_id, runtime.runtime_epoch, manager_runtime_epoch_, accepted.configuration_fence_id,
      accepted.configuration_fence_generation, accepted.captured_scope, ManagerSelectedRuntimeCompositionPhase::kPrepare,
      affected.affected_scope.component_delta, intent.expected_active_projection_revision};
  Require(request.valid(), "configuration preparation formed invalid Host input");
  const auto result = runtime_composition_.InstallOrObserve(request);
  Require(result.valid() && result.request == request,
          "Host returned foreign configuration preparation evidence");
  return {result.state == ManagerSelectedRuntimeCompositionState::kPreparing
              ? ManagerSelectedRuntimeCompositionResultState::kPending
              : ManagerSelectedRuntimeCompositionResultState::kInstalled,
          result, std::nullopt};
}

ManagerSelectedRuntimeCompositionCoordinatorResult
ManagerSelectedRuntimeCompositionCoordinator::PrepareOriginalApplyComponentBringUp(
    const PersistedManagerApplyBringUpPlan &original_plan,
    const core::LogicalSceneSnapshotIdentityDTO &captured_predecessor_snapshot,
    const core::CellAdmissionClaimDTO &original_parent_claim,
    const deployment::RetainedBundle &candidate) {
  std::lock_guard lock(operation_mutex_);
  const auto &plan = original_plan.plan;
  const auto status = apply_journal_.ReadStatus(plan.apply_id, plan.apply_generation);
  Require(status && status->bring_up_plan == original_plan &&
              status->intent.intent_digest == plan.parent_intent_digest &&
              status->affected_scope && status->fence_attempt && status->fence_acceptance &&
              !status->terminal_outcome && !status->private_materialization && !status->publication_receipt,
          "original linked preparation lacks its exact unpublished retained parent");
  const auto &intent = status->intent.intent;
  Require(!intent.parent_authoring_claim && intent.expected_admission_generation &&
              captured_predecessor_snapshot.valid() &&
              captured_predecessor_snapshot == core::LogicalSceneSnapshotIdentityDTO{
                  intent.expected_active_projection_revision.scene_revision, intent.cell_id} &&
              intent.cell_id == apply_journal_.cell_id(),
          "original linked preparation differs from its captured Scene identity");
  const core::CellAdmissionClaimIntentDTO expected_claim{
      core::CellAdmissionClaimKindDTO::kManagerApply, core::CellAdmissionOwnerRoleDTO::kManager,
      intent.request_context.request_owner_epoch, intent.apply_id.value(),
      intent.request_context.request_id, original_plan.plan_digest,
      *intent.expected_admission_generation, {}, std::nullopt, std::nullopt};
  Require(original_parent_claim.intent == expected_claim &&
              core::ValidateCellAdmissionRecoveryReceiptQuery(
                  {expected_claim, original_parent_claim, std::nullopt}).ok(),
          "original linked preparation has a foreign full captured parent claim");
  Require(deployment::verify_retained_bundle_identity(candidate) &&
              candidate.value().cell_graph.cell_id == captured_predecessor_snapshot.cell_id &&
              candidate.retained_bundle_id() == intent.candidate_retained_bundle_id &&
              candidate.value().cell_graph.graph_revision == intent.candidate_graph_revision,
          "original linked preparation candidate differs from its sealed parent");
  const auto bytes = candidate.canonical_bytes();
  Require(!bytes.empty() && bytes.size() <= static_cast<std::size_t>(
              std::numeric_limits<std::uint64_t>::max()),
          "original linked preparation candidate byte extent is invalid");
  const core::ProjectionRevisionDTO target{
      candidate.retained_bundle_id(), candidate.value().cell_graph.graph_revision,
      captured_predecessor_snapshot.revision};
  const ManagerApplyPrivateMaterializationDTO materialization{
      captured_predecessor_snapshot.revision.content_hash, intent.apply_id, intent.apply_generation,
      target, core::ContentDigest(deployment::sha256_digest(bytes)),
      static_cast<std::uint64_t>(bytes.size())};
  Require(materialization.valid() &&
              plan.expected_publication == core::ActiveBundlePublicationRequestDTO{
                  core::NamedApplyPublicationTransactionDTO{intent.apply_id, intent.apply_generation},
                  target, target.retained_bundle_id, target.graph_revision,
                  plan.expected_publication.expected_previous_publication_generation,
                  ComputeManagerApplyPrivateMaterializationDigest(materialization)},
          "original linked preparation materialization differs from its sealed publication");
  std::vector<core::ControllableComponentId> configured;
  for (const auto &row : candidate.value().cell_graph.controllable_components)
    configured.push_back(row.controllable_component_id);
  std::sort(configured.begin(), configured.end());
  Require(configured == plan.configured_components,
          "original linked preparation configured candidate closure differs");
  const auto &runtime = status->latest_runtime_staged_ready;
  Require(runtime && runtime->staged_ready.runtime_units.size() == 1U &&
              runtime->staged_ready.predecessor_publication_generation ==
                  plan.expected_publication.expected_previous_publication_generation &&
              runtime->staged_ready.predecessor_public_pointer_digest == plan.predecessor_public_pointer_digest,
          "original linked preparation lacks its exact staged host/head");
  const auto &runtime_unit = runtime->staged_ready.runtime_units.front();
  for (const auto &child : plan.children)
    Require(child.candidate_projection_revision == target &&
                child.candidate_retained_bundle_digest == materialization.retained_bundle_canonical_content_digest &&
                child.candidate_retained_bundle_bytes == materialization.retained_bundle_canonical_byte_size &&
                child.runtime_unit_id == runtime_unit.runtime_unit_id &&
                child.runtime_epoch == runtime_unit.runtime_epoch && child.manager_runtime_epoch == manager_runtime_epoch_,
            "original linked preparation child differs from the actual candidate or current host epochs");
  const auto &affected = *status->affected_scope;
  const auto &attempt = *status->fence_attempt;
  const auto &accepted = status->fence_acceptance->accepted_fence;
  Require(accepted.matches(attempt.fence_intent) &&
              affected.affected_scope.scope == attempt.fence_intent.scope &&
              affected.affected_scope_digest == attempt.affected_scope_digest &&
              attempt.fence_intent.owner_transaction == core::ApplyFenceOwnerDTO{
                  intent.apply_id, intent.apply_generation},
          "original linked preparation affected scope or accepted fence differs");
  const ManagerSelectedRuntimeCompositionRequestDTO request{
      materialization.authoritative_scene_snapshot_digest, intent.apply_id, intent.apply_generation,
      target, runtime->staged_ready.predecessor_publication_generation,
      runtime->staged_ready.predecessor_public_pointer_digest,
      materialization.retained_bundle_canonical_content_digest,
      materialization.retained_bundle_canonical_byte_size,
      runtime_unit.runtime_unit_id, runtime_unit.runtime_epoch, manager_runtime_epoch_,
      accepted.configuration_fence_id, accepted.configuration_fence_generation, accepted.captured_scope,
      ManagerSelectedRuntimeCompositionPhase::kPrepare, affected.affected_scope.component_delta,
      intent.expected_active_projection_revision};
  Require(request.valid(), "original linked preparation formed invalid Host input");
  const auto result = runtime_composition_.InstallOrObserve(request);
  Require(result.valid() && result.request == request,
          "managed Host returned foreign original linked preparation evidence");
  return {result.state == ManagerSelectedRuntimeCompositionState::kPreparing
              ? ManagerSelectedRuntimeCompositionResultState::kPending
              : ManagerSelectedRuntimeCompositionResultState::kInstalled,
          result, std::nullopt};
}

ManagerSelectedRuntimeCompositionCoordinatorResult
ManagerSelectedRuntimeCompositionCoordinator::InstallOrRecover(
    const core::ApplyId &apply_id,
    const core::ApplyGeneration apply_generation,
    const deployment::RetainedBundle &candidate) {
  return Advance(apply_id, apply_generation, candidate,
                 ManagerSelectedRuntimeCompositionPhase::kFinalize);
}

ManagerSelectedRuntimeCompositionCoordinatorResult
ManagerSelectedRuntimeCompositionCoordinator::PrepareOrObserve(
    const core::ApplyId &apply_id,
    const core::ApplyGeneration apply_generation,
    const deployment::RetainedBundle &candidate) {
  return Advance(apply_id, apply_generation, candidate,
                 ManagerSelectedRuntimeCompositionPhase::kPrepare);
}

ManagerSelectedRuntimeCompositionCoordinatorResult
ManagerSelectedRuntimeCompositionCoordinator::Advance(
    const core::ApplyId &apply_id,
    const core::ApplyGeneration apply_generation,
    const deployment::RetainedBundle &candidate,
    const ManagerSelectedRuntimeCompositionPhase phase) {
  std::lock_guard lock(operation_mutex_);
  const auto apply = apply_journal_.ReadIntent(apply_id, apply_generation);
  const auto attempt =
      apply_journal_.ReadFenceAttempt(apply_id, apply_generation);
  const auto affected =
      apply_journal_.ReadAffectedScope(apply_id, apply_generation);
  const auto acceptance =
      apply_journal_.ReadFenceAcceptance(apply_id, apply_generation);
  const auto runtime =
      apply_journal_.ReadLatestRuntimeStagedReady(apply_id, apply_generation);
  const auto initialized = scene_owner_.ReadMaintenanceInitialization(
      {apply_id, apply_generation});
  Require(apply.has_value() && attempt.has_value() && affected.has_value() &&
              acceptance.has_value() && runtime.has_value() &&
              initialized.has_value(),
          "selected runtime composition requires complete durable Apply, "
          "fence, runtime, and Scene initialization evidence");
  Require(runtime->staged_ready.runtime_units.size() == 1U,
          "N7 selected runtime composition requires the sole staged native "
          "execution host");

  Require(initialized->valid() &&
              (initialized->state ==
                   core::ProjectionMaintenanceInitializationState::kInitialized ||
               initialized->state ==
                   core::ProjectionMaintenanceInitializationState::kCompleted) &&
              core::SameProjectionIdentity(
                  initialized->predecessor_projection_revision,
                  apply->intent.expected_active_projection_revision),
          "selected runtime composition has invalid Scene initialization evidence");
  // Finalize runs before publication (Scene installed, not yet published) and
  // once more after it, while the parent fence is still held, so the runtime
  // owner can verify and adopt the published candidate.
  Require(phase == ManagerSelectedRuntimeCompositionPhase::kPrepare ||
              initialized->ready_for_publication() ||
              (initialized->state ==
                   core::ProjectionMaintenanceInitializationState::kCompleted &&
               initialized->initial_scene_installed),
          "selected runtime composition cannot finalize before Scene is installed");
  const auto &accepted_fence = acceptance->accepted_fence;
  const auto &fence_intent = attempt->fence_intent;
  const auto &runtime_unit = runtime->staged_ready.runtime_units.front();
  Require(
      accepted_fence.matches(fence_intent) &&
          fence_intent.owner_transaction.apply_id == apply_id &&
          fence_intent.owner_transaction.apply_generation == apply_generation &&
          runtime->staged_ready.predecessor_publication_generation.nonzero() &&
          !runtime->staged_ready.predecessor_public_pointer_digest.empty(),
      "selected runtime composition fence or predecessor evidence "
      "conflicts");

  Require(deployment::verify_retained_bundle_identity(candidate) &&
              candidate.value().cell_graph.cell_id == apply_journal_.cell_id() &&
              candidate.value().cell_graph.graph_revision ==
                  initialized->target_projection_revision.graph_revision &&
              candidate.retained_bundle_id() ==
                  apply->intent.candidate_retained_bundle_id,
          "selected runtime composition candidate differs from the durable "
          "Apply");
  const auto canonical_bytes = candidate.canonical_bytes();
  Require(!canonical_bytes.empty() &&
              canonical_bytes.size() <=
                  static_cast<std::size_t>(
                      std::numeric_limits<std::uint64_t>::max()),
          "selected runtime composition bundle byte extent is invalid");
  // The Scene revision recorded by the Apply is evidence of the request; the
  // Scene owner adopts the latest Scene. Prepare and Finalize name the same one.
  const auto &request_scene =
      apply->intent.expected_active_projection_revision.scene_revision;
  const ManagerApplyPrivateMaterializationDTO materialization{
      request_scene.content_hash,
      apply_id,
      apply_generation,
      {apply->intent.candidate_retained_bundle_id,
       apply->intent.candidate_graph_revision, request_scene},
      core::ContentDigest(deployment::sha256_digest(canonical_bytes)),
      static_cast<std::uint64_t>(canonical_bytes.size()),
  };
  Require(materialization.valid(),
          "selected runtime composition materialization is invalid");
  const ManagerSelectedRuntimeCompositionRequestDTO request{
      materialization.authoritative_scene_snapshot_digest,
      materialization.apply_id,
      materialization.apply_generation,
      materialization.candidate_projection_revision,
      runtime->staged_ready.predecessor_publication_generation,
      runtime->staged_ready.predecessor_public_pointer_digest,
      materialization.retained_bundle_canonical_content_digest,
      materialization.retained_bundle_canonical_byte_size,
      runtime_unit.runtime_unit_id,
      runtime_unit.runtime_epoch,
      manager_runtime_epoch_,
      accepted_fence.configuration_fence_id,
      accepted_fence.configuration_fence_generation,
      accepted_fence.captured_scope,
      phase,
      // The exact durable decision, never re-derived here.
      affected->affected_scope.component_delta,
      apply->intent.expected_active_projection_revision,
  };
  Require(affected->affected_scope.scope == fence_intent.scope &&
              attempt->affected_scope_digest ==
                  affected->affected_scope_digest,
          "selected runtime composition fence differs from its durable "
          "affected scope");
  Require(request.valid(), "selected runtime-composition request is invalid");
  const auto result = runtime_composition_.InstallOrObserve(request);
  Require(result.valid() && result.request == request,
          "managed execution host returned foreign runtime-composition "
          "evidence");
  if (result.state == ManagerSelectedRuntimeCompositionState::kPreparing) {
    return {ManagerSelectedRuntimeCompositionResultState::kPending, result,
            std::nullopt};
  }
  if (phase == ManagerSelectedRuntimeCompositionPhase::kPrepare) {
    return {ManagerSelectedRuntimeCompositionResultState::kInstalled, result,
            std::nullopt};
  }
  // Installation is claimed only with live-owner evidence for every
  // retained, added and replaced component, matching the durable delta.
  Require(ComponentReceiptsMatchRequest(request, result.component_receipts),
          "managed execution host installed a component set that differs "
          "from the durable Apply delta");
  auto durable = apply_journal_.EnsurePrivateMaterialization(materialization);
  return {ManagerSelectedRuntimeCompositionResultState::kInstalled, result,
          std::move(durable)};
}

} // namespace manyforge::component_manager
