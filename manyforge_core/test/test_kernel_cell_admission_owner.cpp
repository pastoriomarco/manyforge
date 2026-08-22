#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <manyforge_core/kernel_cell_admission_owner.hpp>
#include <thread>

namespace {

using namespace manyforge::core;

constexpr const char* kDigest =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char* kDigestB =
    "sha256:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

class FixedClock final : public IClock {
 public:
  TimePoint now() const override { return 42; }
};

class AdmissionJournal final : public IOperationJournal {
 public:
  Result<OperationJournalRecoveryDTO> BeginOwnerEpoch(
      const RequestOwnerEpoch&, std::int64_t, std::size_t,
      const std::vector<ResourceId>&) override {
    return Result<OperationJournalRecoveryDTO>::Ok({});
  }
  Result<std::uint64_t> RecordOperationAccepted(
      const RequestOwnerEpoch&, const OperationIntentDTO&, const OperationAcceptedDTO&,
      const OperationStateDTO&) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> RecordControlAccepted(
      const RequestOwnerEpoch&, const OperationControlRequestDTO&,
      const OperationControlAcceptedDTO&, const OperationStateDTO&) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  EmergencyStopAcceptanceProjectionResultDTO RecordStopAccepted(
      const RequestOwnerEpoch&, const EmergencyStopAcceptanceRecordDTO&,
      const EmergencyStopProjectedIntentDTO&, const StopAcceptedDTO&,
      const EmergencyStopAffectedStateSnapshotDTO&) noexcept override {
    return {};
  }
  StopForensicProjectionResultDTO RecordStopTerminal(
      const RequestOwnerEpoch&, const StopStatusDTO&, const StopCertificateDTO&,
      const std::optional<StopFaultLatchDTO>&) noexcept override {
    return {};
  }
  StopForensicProjectionResultDTO RecordStopSurveillanceCheckpoint(
      const RequestOwnerEpoch&, const StopPostTerminalSurveillanceDTO&,
      const StopFaultLatchDTO&, std::int64_t) noexcept override {
    return {};
  }
  Result<std::uint64_t> RecordStopFaultCleared(
      const RequestOwnerEpoch&, const StopFaultRecoveryIntentDTO&,
      const StopFaultRecoveryResultDTO&, const StopFaultLatchDTO&,
      const StopStatusDTO&) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> CloseOwnerEpoch(const RequestOwnerEpoch&, std::int64_t) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordRuntimeScaleAccepted(
      const RequestOwnerEpoch&, const RuntimeScaleRequestDTO&, const RuntimeScaleAcceptedDTO&,
      const OperationStateDTO&, const RuntimeScaleStatusDTO&) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> RecordOperationState(
      const RequestOwnerEpoch&, const OperationStateDTO&, const RuntimeScaleStatusDTO*,
      const char*) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> RecordCellAdmissionClaimAcquired(
      const RequestOwnerEpoch&, const CellAdmissionClaimDTO&) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordCellAdmissionClaimReleased(
      const RequestOwnerEpoch&, const CellAdmissionClaimDTO&,
      const CellAdmissionClaimReleaseDTO&, AdmissionGeneration) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordCellAdmissionClaimsReconciled(
      const RequestOwnerEpoch&, const CellAdmissionReconcileDTO&,
      const CellAdmissionReconcileValidationDTO&, AdmissionGeneration) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordCellAdmissionGenerationAdvanced(
      const RequestOwnerEpoch&, AdmissionGeneration, AdmissionGeneration) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }

  std::atomic<std::uint64_t> sequence{};
};

CellAdmissionReadinessSnapshotDTO Ready() {
  return CellAdmissionReadinessSnapshotDTO{
      true,
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 1U,
                            ProgramCycleStateDTO::kIdleAfterReset, std::nullopt, std::nullopt,
                            std::nullopt},
      true, true, true, 2U, 2U,
      true, true, 0U, 0U, 0U};
}

CellAdmissionOwnerRoleDTO Role(CellAdmissionClaimKindDTO kind) {
  switch (kind) {
    case CellAdmissionClaimKindDTO::kProgramCycle:
      return CellAdmissionOwnerRoleDTO::kProgramRunner;
    case CellAdmissionClaimKindDTO::kAuthoringMutation:
      return CellAdmissionOwnerRoleDTO::kAuthoringOwner;
    case CellAdmissionClaimKindDTO::kManagerApply:
      return CellAdmissionOwnerRoleDTO::kManager;
    case CellAdmissionClaimKindDTO::kComponentLifecycle:
      return CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner;
    case CellAdmissionClaimKindDTO::kAuthoredScene:
      return CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner;
    case CellAdmissionClaimKindDTO::kManualOperation:
      return CellAdmissionOwnerRoleDTO::kManualOperationOwner;
    case CellAdmissionClaimKindDTO::kInterventionOrRecovery:
      return CellAdmissionOwnerRoleDTO::kInterventionOrRecoveryOwner;
  }
  return CellAdmissionOwnerRoleDTO::kAuthoringOwner;
}

CellAdmissionClaimIntentDTO Intent(CellAdmissionClaimKindDTO kind, AdmissionGeneration generation,
                                   std::string request = "request.1") {
  CellAdmissionClaimIntentDTO intent{
      kind, Role(kind), RequestOwnerEpoch("owner.1"), "transaction.1",
      RequestId(std::move(request)), ContentDigest(kDigest), generation, {}, std::nullopt,
      std::nullopt};
  if (kind == CellAdmissionClaimKindDTO::kProgramCycle) {
    intent.program_cycle_evidence =
        ProgramCycleAdmissionEvidenceDTO{ProgramRevision("program.revision"),
                                         ProgramRevision("program.revision")};
  }
  if (kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery) {
    intent.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
        CellAdmissionOwnerRoleDTO::kProgramRunner, RequestOwnerEpoch("program.owner"),
        "offer.1", ContentDigest(kDigest), std::nullopt};
  }
  return intent;
}

CellAdmissionClaimReleaseDTO Release(const CellAdmissionClaimDTO& claim) {
  const auto settlement =
      claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle ||
              claim.intent.claim_kind == CellAdmissionClaimKindDTO::kManualOperation
          ? CellAdmissionSettlementKindDTO::kExplicitReset
          : (claim.intent.claim_kind == CellAdmissionClaimKindDTO::kAuthoringMutation ||
                     claim.intent.claim_kind == CellAdmissionClaimKindDTO::kAuthoredScene
                 ? CellAdmissionSettlementKindDTO::kOwnerCommit
                 : CellAdmissionSettlementKindDTO::kTerminalEffectsSettled);
  return CellAdmissionClaimReleaseDTO{
      claim.intent.owner_role,
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      CellAdmissionSettlementEvidenceDTO{
          settlement, ContentDigest(kDigest)}};
}

KernelCellAdmissionOwner Owner(FixedClock& clock, AdmissionJournal& journal,
                               std::size_t capacity = 16U) {
  return KernelCellAdmissionOwner(
      RequestOwnerEpoch("kernel.1"), capacity, clock, journal, {},
      AdmissionGeneration(0U),
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto& reconcile) {
        return Result<CellAdmissionReconcileValidationDTO>::Ok(
            {reconcile.current_owner_epochs, {}});
      });
}

TEST(KernelCellAdmissionOwner, StartsFalseAndEveryReadinessInputFailsClosed) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  const auto ready = Ready();
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  const auto durable_sequence = journal.sequence.load();
  EXPECT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);
  const auto generation = owner.GetAuthoringAdmission().admission_generation;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  EXPECT_EQ(owner.GetAuthoringAdmission().admission_generation, generation);
  EXPECT_EQ(journal.sequence.load(), durable_sequence);

  for (int row = 0; row != 13; ++row) {
    auto blocked = ready;
    switch (row) {
      case 0: blocked.program_cycle_status_current = false; break;
      case 1:
        blocked.program_cycle_status = ProgramCycleStatusDTO{
            RequestOwnerEpoch("program.owner"), 2U,
            ProgramCycleStateDTO::kTerminalAwaitingReset, ProgramRunId("run.1"),
            CellAdmissionClaimGeneration(1U), ContentDigest(kDigest)};
        break;
      case 2: blocked.manager_status_current = false; break;
      case 3: blocked.manager_current = false; break;
      case 4: blocked.required_component_status_current = false; break;
      case 5: blocked.required_component_total = 3U; break;
      case 6: blocked.required_component_current_compatible_ready_converged = 1U; break;
      case 7: blocked.deployment_status_current = false; break;
      case 8: blocked.deployment_converged = false; break;
      case 9: blocked.blocking_fault_count = 1U; break;
      case 10: blocked.unresolved_owner_count = 1U; break;
      case 11: blocked.unsettled_effect_count = 1U; break;
      case 12:
        blocked.program_cycle_status = ProgramCycleStatusDTO{
            RequestOwnerEpoch("program.owner"), 2U, ProgramCycleStateDTO::kRunning,
            ProgramRunId("run.1"), CellAdmissionClaimGeneration(1U), std::nullopt};
        break;
    }
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(blocked).ok());
    EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed) << row;
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  }
}

TEST(KernelCellAdmissionOwner, ClosedFreshMatrixAndReplayReleaseFences) {
  constexpr CellAdmissionClaimKindDTO kinds[] = {
      CellAdmissionClaimKindDTO::kProgramCycle,
      CellAdmissionClaimKindDTO::kAuthoringMutation,
      CellAdmissionClaimKindDTO::kManagerApply,
      CellAdmissionClaimKindDTO::kComponentLifecycle,
      CellAdmissionClaimKindDTO::kAuthoredScene,
      CellAdmissionClaimKindDTO::kManualOperation};
  for (const auto kind : kinds) {
    FixedClock clock;
    AdmissionJournal journal;
    auto owner = Owner(clock, journal);
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    const auto generation = owner.GetAuthoringAdmission().admission_generation;
    auto accepted = owner.AcquireCellAdmissionClaim(Intent(kind, generation));
    ASSERT_TRUE(accepted.ok()) << to_string(kind);
    EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
    auto replay = owner.AcquireCellAdmissionClaim(Intent(kind, generation));
    ASSERT_TRUE(replay.ok());
    EXPECT_EQ(replay.value(), accepted.value());
    auto conflicting = Intent(kind, generation);
    conflicting.owner_transaction_id = "transaction.conflict";
    EXPECT_EQ(owner.AcquireCellAdmissionClaim(conflicting).error().code, "request_id_conflict");
    EXPECT_EQ(owner.AcquireCellAdmissionClaim(
                  Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                         owner.GetAuthoringAdmission().admission_generation, "request.2"))
                  .error()
                  .code,
              "authoring_not_ready");
    const auto not_ready = owner.AcquireCellAdmissionClaim(
        Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
               owner.GetAuthoringAdmission().admission_generation, "request.details"));
    ASSERT_FALSE(not_ready.ok());
    EXPECT_EQ(not_ready.error().details.size(), 4U);
    EXPECT_EQ(not_ready.error().details.at("admission_generation"),
              std::to_string(owner.GetAuthoringAdmission().admission_generation.value()));
    EXPECT_EQ(not_ready.error().details.at("retryable"), "true");
    EXPECT_EQ(not_ready.error().details.at("required_action"),
              "inspect_owner_status_and_restore_authoring_readiness");
    auto release = Release(accepted.value());
    ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(release).ok());
    EXPECT_EQ(owner.ReleaseCellAdmissionClaim(release).error().code, "stale_generation");
  }
}

TEST(KernelCellAdmissionOwner, InterventionCoexistsOnlyWithExactParentCycle) {
  {
    FixedClock standalone_clock;
    AdmissionJournal standalone_journal;
    auto standalone = Owner(standalone_clock, standalone_journal);
    EXPECT_TRUE(standalone
                    .AcquireCellAdmissionClaim(Intent(
                        CellAdmissionClaimKindDTO::kInterventionOrRecovery,
                        standalone.GetAuthoringAdmission().admission_generation))
                    .ok());
  }
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto cycle = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kProgramCycle,
             owner.GetAuthoringAdmission().admission_generation));
  ASSERT_TRUE(cycle.ok());
  auto intervention =
      Intent(CellAdmissionClaimKindDTO::kInterventionOrRecovery,
             owner.GetAuthoringAdmission().admission_generation, "request.recovery");
  intervention.intervention_evidence->parent_program_cycle_claim_generation =
      cycle.value().claim_generation;
  EXPECT_TRUE(owner.AcquireCellAdmissionClaim(intervention).ok());

  FixedClock conflict_clock;
  AdmissionJournal conflict_journal;
  auto conflict = Owner(conflict_clock, conflict_journal);
  ASSERT_TRUE(conflict.UpdateCellAdmissionReadiness(Ready()).ok());
  ASSERT_TRUE(conflict
                  .AcquireCellAdmissionClaim(Intent(
                      CellAdmissionClaimKindDTO::kAuthoringMutation,
                      conflict.GetAuthoringAdmission().admission_generation))
                  .ok());
  EXPECT_EQ(conflict
                .AcquireCellAdmissionClaim(Intent(
                    CellAdmissionClaimKindDTO::kInterventionOrRecovery,
                    conflict.GetAuthoringAdmission().admission_generation,
                    "request.foreign-recovery"))
                .error()
                .code,
            "authoring_not_ready");
}

TEST(KernelCellAdmissionOwner, RunAndAuthoringRaceHasOneWinner) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const auto generation = owner.GetAuthoringAdmission().admission_generation;
  std::barrier start(3);
  std::atomic<int> accepted{};
  auto acquire = [&](CellAdmissionClaimKindDTO kind, const char* request) {
    start.arrive_and_wait();
    if (owner.AcquireCellAdmissionClaim(Intent(kind, generation, request)).ok()) {
      ++accepted;
    }
  };
  std::thread run(acquire, CellAdmissionClaimKindDTO::kProgramCycle, "request.run");
  std::thread author(acquire, CellAdmissionClaimKindDTO::kAuthoringMutation, "request.author");
  start.arrive_and_wait();
  run.join();
  author.join();
  EXPECT_EQ(accepted.load(), 1);
}

TEST(KernelCellAdmissionOwner, CapacityAndForeignOrStaleReleaseFailClosed) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal, 1U);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto claim = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
             owner.GetAuthoringAdmission().admission_generation));
  ASSERT_TRUE(claim.ok());
  auto stale_intent = Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                             AdmissionGeneration(1U), "request.stale");
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(stale_intent).error().code, "stale_generation");
  auto foreign = Release(claim.value());
  foreign.owner_epoch = RequestOwnerEpoch("foreign.owner");
  EXPECT_EQ(owner.ReleaseCellAdmissionClaim(foreign).error().code, "stale_generation");
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(claim.value())).ok());
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(claim.value().intent).error().code,
            "request_result_expired");
  auto conflicting = claim.value().intent;
  conflicting.owner_transaction_id = "conflicting.transaction";
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(conflicting).error().code,
            "request_id_conflict");
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(
                Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                       owner.GetAuthoringAdmission().admission_generation, "request.2"))
                .error()
                .code,
            "request_ledger_full");
}

TEST(KernelCellAdmissionOwner, ReleaseUsesImmutableAcquireGenerationNotCurrentGeneration) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto claim = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
             owner.GetAuthoringAdmission().admission_generation));
  ASSERT_TRUE(claim.ok());
  auto changed = Ready();
  changed.manager_current = false;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(changed).ok());
  EXPECT_NE(owner.GetAuthoringAdmission().admission_generation,
            claim.value().acquired_admission_generation);
  EXPECT_TRUE(owner.ReleaseCellAdmissionClaim(Release(claim.value())).ok());
}

TEST(KernelCellAdmissionOwner,
     ReconcileRequiresCompleteExactInventoryAndValidatorAuthorizedSettlement) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto first_intent =
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, AdmissionGeneration(7U));
  auto second_intent =
      Intent(CellAdmissionClaimKindDTO::kManagerApply, AdmissionGeneration(8U), "request.2");
  second_intent.owner_epoch = RequestOwnerEpoch("manager.owner.1");
  second_intent.owner_transaction_id = "apply.transaction.1";
  const CellAdmissionClaimDTO first{
      first_intent, CellAdmissionClaimGeneration(4U), AdmissionGeneration(7U), 41};
  const CellAdmissionClaimDTO second{
      second_intent, CellAdmissionClaimGeneration(5U), AdmissionGeneration(8U), 42};
  const std::vector<OperationJournalRecoveryDTO::RecoveredCellAdmissionClaimDTO> recovery{
      {first, std::nullopt}, {second, std::nullopt}};
  const std::vector<CellAdmissionCurrentOwnerEpochDTO> successors{
      {CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.2")},
      {CellAdmissionOwnerRoleDTO::kManager, RequestOwnerEpoch("manager.owner.1")}};
  const auto make_owner = [&](bool authorize_first) {
    return KernelCellAdmissionOwner(
        RequestOwnerEpoch("kernel.2"), 4U, clock, journal, recovery,
        AdmissionGeneration(8U), {},
        [](const auto&, const auto&) { return Result<void>::Ok(); },
        [&, authorize_first](const auto& held, const auto& reconcile) {
          CellAdmissionReconcileValidationDTO validation{
              reconcile.current_owner_epochs, {}};
          if (authorize_first) {
            validation.settlements.push_back(
                {held.front(), CellAdmissionOwnerRoleDTO::kAuthoringOwner,
                 RequestOwnerEpoch("owner.2"),
                 CellAdmissionSettlementKindDTO::kReconciled,
                 reconcile.reconciliation_evidence_digest});
          }
          return Result<CellAdmissionReconcileValidationDTO>::Ok(validation);
        },
        {},
        {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.1")},
         {CellAdmissionOwnerRoleDTO::kManager, RequestOwnerEpoch("manager.owner.1")}});
  };

  CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel.2"), {first, second}, successors, ContentDigest(kDigest)};

  auto byte_different = make_owner(false);
  auto changed_inventory = reconcile;
  changed_inventory.retained_claims.front().accepted_at_ns = 43;
  const auto rejected_bytes =
      byte_different.ReconcileCellAdmissionClaims(changed_inventory);
  ASSERT_FALSE(rejected_bytes.ok());
  EXPECT_EQ(rejected_bytes.error().details.at("reason"),
            "reconcile_inventory_not_exact");

  auto incomplete = make_owner(false);
  auto omitted = reconcile;
  omitted.retained_claims.pop_back();
  EXPECT_EQ(incomplete.ReconcileCellAdmissionClaims(omitted)
                .error()
                .details.at("reason"),
            "reconcile_inventory_not_exact");

  auto raw_successor_only = make_owner(false);
  const auto raw_rotation = raw_successor_only.ReconcileCellAdmissionClaims(reconcile);
  ASSERT_FALSE(raw_rotation.ok());
  EXPECT_EQ(raw_rotation.error().details.at("reason"),
            "reconcile_predecessor_unsettled");
  EXPECT_TRUE(raw_successor_only.has_unresolved_claims());

  auto stale_release = KernelCellAdmissionOwner(
      RequestOwnerEpoch("kernel.2"), 4U, clock, journal, recovery,
      AdmissionGeneration(8U), {},
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      {}, {}, successors);
  EXPECT_EQ(stale_release.ReleaseCellAdmissionClaim(Release(first)).error().code,
            "stale_generation");

  auto settled = make_owner(true);
  ASSERT_TRUE(settled.ReconcileCellAdmissionClaims(reconcile).ok());
  EXPECT_TRUE(settled.has_unresolved_claims());
  EXPECT_TRUE(settled.ReconcileCellAdmissionClaims(reconcile).ok());
  EXPECT_EQ(settled.ReleaseCellAdmissionClaim(Release(first)).error().code,
            "stale_generation");
  EXPECT_TRUE(settled.ReleaseCellAdmissionClaim(Release(second)).ok());
}

TEST(KernelCellAdmissionOwner, SupportsMultipleExactReconciliationsInOneKernelEpoch) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto intent =
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, AdmissionGeneration(7U));
  const CellAdmissionClaimDTO claim{
      intent, CellAdmissionClaimGeneration(4U), AdmissionGeneration(7U), 41};
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.2"), 4U, clock, journal,
      {{claim, std::nullopt}}, AdmissionGeneration(8U), {}, {},
      [&](const auto& held, const auto& reconcile) {
        CellAdmissionReconcileValidationDTO validation{
            reconcile.current_owner_epochs, {}};
        if (reconcile.reconciliation_evidence_digest == ContentDigest(kDigestB)) {
          validation.settlements.push_back(
              {held.front(), CellAdmissionOwnerRoleDTO::kAuthoringOwner,
               RequestOwnerEpoch("owner.2"),
               CellAdmissionSettlementKindDTO::kReconciled,
               reconcile.reconciliation_evidence_digest});
        }
        return Result<CellAdmissionReconcileValidationDTO>::Ok(validation);
      },
      {},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.1")}});

  const CellAdmissionReconcileDTO first{
      RequestOwnerEpoch("kernel.2"), {claim},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.1")}},
      ContentDigest(kDigest)};
  ASSERT_TRUE(owner.ReconcileCellAdmissionClaims(first).ok());
  ASSERT_TRUE(owner.ReconcileCellAdmissionClaims(first).ok());

  const CellAdmissionReconcileDTO second{
      RequestOwnerEpoch("kernel.2"), {claim},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.2")}},
      ContentDigest(kDigestB)};
  ASSERT_TRUE(owner.ReconcileCellAdmissionClaims(second).ok());
  ASSERT_TRUE(owner.ReconcileCellAdmissionClaims(second).ok());
  EXPECT_FALSE(owner.has_unresolved_claims());
  EXPECT_EQ(owner.ReleaseCellAdmissionClaim(Release(claim)).error().code,
            "stale_generation");
}

}  // namespace
