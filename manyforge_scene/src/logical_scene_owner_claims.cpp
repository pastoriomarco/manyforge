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
    const SceneTransactionRequestDTO &request,
    const ContentDigest &request_digest) {
  if (!request.expected_admission_generation.has_value()) {
    throw LogicalSceneOwnerConflictError(
        "authored scene mutation lacks its admission generation");
  }
  core::CellAdmissionClaimIntentDTO intent{
      core::CellAdmissionClaimKindDTO::kAuthoredScene,
      core::CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner,
      RequestOwnerEpoch(
          request.expected_projection_revision.scene_revision.scene_id.value()),
      request.request_context.request_id.value(),
      // Scope replay to this exact request, including its rotating request
      // epoch.
      core::RequestId(request_digest.value()),
      request_digest,
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
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
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
  auto intent = AuthoredSceneClaimIntent(request, request_digest);
  auto receipt = admission.GetCellAdmissionRecoveryReceipt(
      {intent, std::nullopt, std::nullopt});
  if (receipt.ok() &&
      receipt.value().status ==
          core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown) {
    // Pre-upgrade claims used the request epoch. Only recover that exact
    // intent; new claims always use the stable Scene identity.
    intent.owner_epoch = request.request_context.request_owner_epoch;
    intent.request_id = request.request_context.request_id;
    receipt = admission.GetCellAdmissionRecoveryReceipt(
        {intent, std::nullopt, std::nullopt});
  }
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

void SettleRecoveredAuthoredSceneClaims(
    sqlite3 *database, core::IKernelCellAdmissionPort &admission) {
  Statement rows(
      database,
      "SELECT i.request_json,i.request_digest,o.success,o.projection_status "
      "FROM logical_scene_transaction_intents i "
      "JOIN logical_scene_request_outcomes o "
      "ON o.request_owner_epoch=i.request_owner_epoch "
      "AND o.request_id=i.request_id ORDER BY i.transaction_sequence");
  while (rows.Step()) {
    const auto request =
        logical_scene_codec_internal::DecodeRequest(rows.Text(0));
    const auto success = rows.Integer(2) == 1;
    SettleAuthoredSceneClaimIfHeld(
        admission, request, ContentDigest(rows.Text(1)), success,
        DecodeProjectionStatus(rows.Text(3)));
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
    SettleRecoveredAuthoredSceneClaims(impl_->database(),
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

core::Result<void> ValidateAuthoredSceneCellAdmissionSettlementReadOnly(
    const std::filesystem::path &database_path, const core::CellId &cell_id,
    const core::CellAdmissionClaimDTO &claim,
    const core::CellAdmissionClaimReleaseDTO &release) {
  const auto rejected = [](std::string reason) {
    return core::Result<void>::Err(core::ErrorDTO{
        "invariant_violation",
        "authored-scene cell-admission settlement is not owner-proven",
        {{"reason", std::move(reason)}}});
  };
  if (!core::ValidateCellAdmissionClaimIntent(claim.intent).ok() ||
      claim.intent.claim_kind !=
          core::CellAdmissionClaimKindDTO::kAuthoredScene ||
      claim.intent.owner_role !=
          core::CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner ||
      release.owner_role != claim.intent.owner_role ||
      release.owner_epoch != claim.intent.owner_epoch ||
      release.owner_transaction_id != claim.intent.owner_transaction_id ||
      release.request_id != claim.intent.request_id ||
      release.request_payload_digest != claim.intent.request_payload_digest ||
      release.acquired_admission_generation !=
          claim.acquired_admission_generation ||
      release.claim_generation != claim.claim_generation) {
    return rejected("authored_scene_release_identity_mismatch");
  }

  sqlite3 *database = nullptr;
  const auto opened = sqlite3_open_v2(
      database_path.string().c_str(), &database,
      SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
      nullptr);
  if (opened != SQLITE_OK) {
    if (database != nullptr) {
      sqlite3_close(database);
    }
    return rejected("logical_scene_journal_unavailable");
  }
  try {
    Check(database, sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds),
          "configure read-only scene-settlement timeout");
    Exec(database, "PRAGMA query_only=ON");
    Transaction snapshot(database, "BEGIN");
    if (ScalarInteger(database, "PRAGMA application_id") !=
            kCanonicalEmptySceneSqliteApplicationId ||
        ScalarInteger(database, "PRAGMA user_version") !=
            kLogicalSceneStoreSchemaVersion) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene settlement reader found a foreign schema");
    }
    Statement metadata(
        database,
        "SELECT cell_id FROM logical_scene_store_metadata WHERE singleton=1");
    if (!metadata.Step() || metadata.Text(0) != cell_id.value() ||
        metadata.Step()) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene settlement reader found a foreign cell");
    }
    Statement row(
        database,
        "SELECT i.request_json,i.request_digest,o.success,o.projection_status "
        "FROM logical_scene_transaction_intents i "
        "JOIN logical_scene_request_outcomes o "
        "ON o.request_owner_epoch=i.request_owner_epoch "
        "AND o.request_id=i.request_id "
        "WHERE i.request_digest=?");
    row.Bind(1, claim.intent.request_payload_digest.value());
    if (!row.Step()) {
      throw LogicalSceneOwnerIntegrityError(
          "authored-scene terminal outcome is absent");
    }
    const auto request =
        logical_scene_codec_internal::DecodeRequest(row.Text(0));
    const auto request_digest = row.Text(1);
    const bool success = row.Integer(2) == 1;
    const auto projection_status = DecodeProjectionStatus(row.Text(3));
    if (row.Step()) {
      throw LogicalSceneOwnerIntegrityError(
          "authored-scene terminal outcome is not unique");
    }
    snapshot.Commit();
    sqlite3_close(database);
    database = nullptr;
    const auto expected_kind =
        success ? core::CellAdmissionSettlementKindDTO::kOwnerCommit
                : core::CellAdmissionSettlementKindDTO::kProvenNoEffect;
    const auto intent =
        AuthoredSceneClaimIntent(request, ContentDigest(request_digest));
    auto legacy_intent = intent;
    legacy_intent.owner_epoch = request.request_context.request_owner_epoch;
    legacy_intent.request_id = request.request_context.request_id;
    if ((!IsAuthoredSceneMutation(request) &&
         !request.program_node_evidence.has_value()) ||
        (claim.intent != intent && claim.intent != legacy_intent) ||
        request_digest != claim.intent.request_payload_digest.value() ||
        release.settlement.evidence_digest.value() != request_digest ||
        release.settlement.settlement_kind != expected_kind ||
        (!success &&
         projection_status !=
             SceneProjectionCommitStatusDTO::kRejectedRollbackProven &&
         projection_status != SceneProjectionCommitStatusDTO::kNotAttempted)) {
      return rejected("authored_scene_terminal_evidence_mismatch");
    }
    return core::Result<void>::Ok();
  } catch (const std::exception &) {
    sqlite3_close(database);
    return rejected("logical_scene_journal_read_failed");
  }
}

} // namespace manyforge::scene
