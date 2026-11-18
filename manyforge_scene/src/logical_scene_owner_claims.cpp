#include "logical_scene_owner_impl.hpp"

namespace manyforge::scene::logical_scene_owner_detail {

[[nodiscard]] bool IsAuthoredSceneMutation(
    const SceneTransactionRequestDTO &request) {
  if (std::holds_alternative<core::AddObjectRequestDTO>(request.operation) ||
      std::holds_alternative<core::UpsertObjectRequestDTO>(request.operation) ||
      std::holds_alternative<core::RemoveObjectRequestDTO>(request.operation) ||
      std::holds_alternative<core::SetCollisionPolicyRequestDTO>(
          request.operation)) {
    return true;
  }
  if (const auto *update =
          std::get_if<core::UpdateObjectPoseRequestDTO>(&request.operation)) {
    return !update->provider_evidence.has_value();
  }
  return false;
}

[[nodiscard]] core::CellAdmissionClaimIntentDTO AuthoredSceneClaimIntent(
    const SceneTransactionRequestDTO &request) {
  if (!request.expected_admission_generation.has_value()) {
    throw LogicalSceneOwnerConflictError(
        "authored scene mutation lacks its admission generation");
  }
  core::CellAdmissionClaimIntentDTO intent{
      core::CellAdmissionClaimKindDTO::kAuthoredScene,
      core::ApplicationRole::kAuthoredSceneOwner,
      std::nullopt,
      request.request_context.request_key,
      *request.expected_admission_generation,
      {},
      std::nullopt,
      std::nullopt,
      request.program_node_evidence,
  };
  if (!core::ValidateCellAdmissionClaimIntent(intent).ok()) {
    throw LogicalSceneOwnerIntegrityError(
        "authored scene mutation formed an invalid admission intent");
  }
  return intent;
}

void ReleaseAuthoredSceneClaim(
    core::IKernelCellAdmissionPort &admission,
    const core::CellAdmissionClaimDTO &claim,
    const ContentDigest &terminal_evidence,
    const core::CellAdmissionSettlementKindDTO settlement_kind) {
  const core::CellAdmissionClaimReleaseDTO release{
      claim.intent.owner_role,
      claim.intent.owner_transaction_id,
      claim.intent.request_key,
      claim.acquired_admission_generation,
      claim.claim_generation,
      {settlement_kind, terminal_evidence},
  };
  const auto released = admission.ReleaseCellAdmissionClaim(release);
  if (!released.ok()) {
    ThrowCellAdmissionFailure("authored scene settlement was rejected",
                              released.error());
  }
}

void SettleAuthoredSceneClaimIfHeld(
    core::IKernelCellAdmissionPort &admission,
    const SceneTransactionRequestDTO &request,
    const ContentDigest &request_digest, const bool success,
    const SceneProjectionCommitStatusDTO projection_status) {
  if ((!IsAuthoredSceneMutation(request) &&
       !request.program_node_evidence.has_value()) ||
      !request.expected_admission_generation.has_value() ||
      (!success &&
       projection_status !=
           SceneProjectionCommitStatusDTO::kRejectedRollbackProven &&
       projection_status != SceneProjectionCommitStatusDTO::kNotAttempted)) {
    return;
  }
  const auto intent = AuthoredSceneClaimIntent(request);
  const auto receipt = admission.GetCellAdmissionRecoveryReceipt(
      {intent, std::nullopt, std::nullopt});
  if (!receipt.ok()) {
    ThrowCellAdmissionFailure("authored scene recovery lookup failed",
                              receipt.error());
  }
  if (receipt.value().status ==
      core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown) {
    return;
  }
  if (receipt.value().status ==
      core::CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact) {
    return;
  }
  if (receipt.value().status !=
          core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
      !receipt.value().matched_claim.has_value()) {
    throw LogicalSceneOwnerConflictError(
        "authored scene recovery claim is conflicting");
  }
  ReleaseAuthoredSceneClaim(
      admission, *receipt.value().matched_claim, request_digest,
      success ? core::CellAdmissionSettlementKindDTO::kOwnerCommit
              : core::CellAdmissionSettlementKindDTO::kProvenNoEffect);
}

// Every resident intent with a recorded outcome, in transaction order; an
// already released or unknown claim is left as it is.
void SettleRecoveredAuthoredSceneClaims(
    const SceneStoreMemory &memory, core::IKernelCellAdmissionPort &admission) {
  for (const auto &intent : memory.transactions) {
    const auto outcome = memory.outcomes.find(intent.request_key);
    if (outcome == memory.outcomes.end()) {
      continue;
    }
    const auto request =
        logical_scene_codec_internal::DecodeRequest(intent.request_json);
    SettleAuthoredSceneClaimIfHeld(admission, request, intent.request_digest,
                                   outcome->second.success,
                                   outcome->second.projection_status);
  }
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene {

core::Result<void>
LogicalSceneOwner::ReconcileAuthoredSceneAdmissionClaims() {
  std::lock_guard lock(impl_->mutex_);
  if (impl_->cell_admission_ == nullptr) {
    return core::Result<void>::Err(
        {"kernel_owner_unavailable",
         "authored scene admission owner is not configured", {}});
  }
  try {
    impl_->RequireOpen();
    SettleRecoveredAuthoredSceneClaims(impl_->memory_,
                                       *impl_->cell_admission_);
    return core::Result<void>::Ok();
  } catch (const CellAdmissionTemporarilyUnavailable &error) {
    return core::Result<void>::Err(
        {"kernel_owner_unavailable", error.what(), {}});
  } catch (const std::exception &error) {
    return core::Result<void>::Err(
        {"authored_scene_admission_reconciliation_failed", error.what(), {}});
  } catch (...) {
    return core::Result<void>::Err(
        {"authored_scene_admission_reconciliation_failed",
         "authored scene admission reconciliation raised a non-standard "
         "exception",
         {}});
  }
}

} // namespace manyforge::scene
