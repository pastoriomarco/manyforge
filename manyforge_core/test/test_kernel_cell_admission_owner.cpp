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
  Result<OperationJournalRecoveryDTO> BeginOwnerEpoch(const RequestOwnerEpoch&, std::int64_t,
                                                      std::size_t,
                                                      const std::vector<ResourceId>&) override {
    return Result<OperationJournalRecoveryDTO>::Ok({});
  }
  Result<std::uint64_t> RecordOperationAccepted(const RequestOwnerEpoch&, const OperationIntentDTO&,
                                                const OperationAcceptedDTO&,
                                                const OperationStateDTO&,
                                                std::span<const OperationId>) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> RecordControlAccepted(const RequestOwnerEpoch&,
                                              const OperationControlRequestDTO&,
                                              const OperationControlAcceptedDTO&,
                                              const OperationStateDTO&) override {
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
      const RequestOwnerEpoch&, const StopPostTerminalSurveillanceDTO&, const StopFaultLatchDTO&,
      std::int64_t) noexcept override {
    return {};
  }
  Result<std::uint64_t> RecordStopFaultCleared(const RequestOwnerEpoch&,
                                               const StopFaultRecoveryIntentDTO&,
                                               const StopFaultRecoveryResultDTO&,
                                               const StopFaultLatchDTO&,
                                               const StopStatusDTO&) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> CloseOwnerEpoch(const RequestOwnerEpoch&, std::int64_t) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordRuntimeScaleAccepted(const RequestOwnerEpoch&,
                                                   const RuntimeScaleRequestDTO&,
                                                   const RuntimeScaleAcceptedDTO&,
                                                   const OperationStateDTO&,
                                                   const RuntimeScaleStatusDTO&) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> RecordOperationState(const RequestOwnerEpoch&, const OperationStateDTO&,
                                             const RuntimeScaleStatusDTO*, const char*,
                                             std::span<const PreparedMotionDiagnosticsDTO>) override {
    return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
  }
  Result<std::uint64_t> RecordCellAdmissionClaimAcquired(const RequestOwnerEpoch&,
                                                         const CellAdmissionClaimDTO&) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordCellAdmissionClaimReleased(const RequestOwnerEpoch&,
                                                         const CellAdmissionClaimDTO&,
                                                         const CellAdmissionClaimReleaseDTO&,
                                                         AdmissionGeneration) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordCellAdmissionClaimsReconciled(
      const RequestOwnerEpoch&, const CellAdmissionReconcileDTO&,
      const CellAdmissionReconcileValidationDTO&, AdmissionGeneration) override {
    return Result<std::uint64_t>::Ok(++sequence);
  }
  Result<std::uint64_t> RecordCellAdmissionGenerationAdvanced(const RequestOwnerEpoch&,
                                                              AdmissionGeneration,
                                                              AdmissionGeneration) override {
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
      true,
      2U,
      2U,
      true,
      true,
      0U,
      0U,
      0U};
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
  CellAdmissionClaimIntentDTO intent{kind,
                                     Role(kind),
                                     RequestOwnerEpoch("owner.1"),
                                     "transaction.1",
                                     RequestId(std::move(request)),
                                     ContentDigest(kDigest),
                                     generation,
                                     {},
                                     std::nullopt,
                                     std::nullopt};
  if (kind == CellAdmissionClaimKindDTO::kProgramCycle) {
    intent.program_cycle_evidence = ProgramCycleAdmissionEvidenceDTO{
        ProgramRevision("program.revision"), ProgramRevision("program.revision")};
  }
  if (kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery) {
    intent.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
        CellAdmissionOwnerRoleDTO::kProgramRunner, RequestOwnerEpoch("program.owner"), "offer.1",
        ContentDigest(kDigest), std::nullopt};
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
      CellAdmissionSettlementEvidenceDTO{settlement, ContentDigest(kDigest)}};
}

ContentDigest Digest(char digit) {
  return ContentDigest("sha256:" + std::string(64U, digit));
}

ManagerLivenessRequiredSetDTO EmptyLivenessSet() {
  ManagerLivenessRequiredSetDTO result{CellId("cell.1"),
                                       RetainedBundleId("bundle.1"),
                                       ContentDigest(kDigest),
                                       RuntimeUnitId("manager"),
                                       RuntimeEpoch("manager.epoch.1"),
                                       RequestOwnerEpoch("manager.owner.1"),
                                       {}};
  result.required_runtime_set_digest = ComputeRequiredRuntimeSetDigest(result.required_receivers);
  return result;
}

KernelCellAdmissionOwner Owner(FixedClock& clock, AdmissionJournal& journal,
                               std::size_t capacity = 16U) {
  return KernelCellAdmissionOwner(
      RequestOwnerEpoch("kernel.1"), capacity, clock, journal, {}, AdmissionGeneration(0U),
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&, const auto& reconcile) {
        return Result<CellAdmissionReconcileValidationDTO>::Ok(
            {reconcile.current_owner_epochs, {}});
      },
      {}, {}, EmptyLivenessSet());
}

ManagerLivenessRequiredSetDTO LivenessSet(
    std::vector<RequiredManagerLivenessReceiverDTO> receivers =
        {{RuntimeUnitId("runtime.a"), RuntimeEpoch("runtime.a.epoch"), RuntimePolicyId("policy.1"),
          RuntimePolicyRevision("1")},
         {RuntimeUnitId("runtime.b"), RuntimeEpoch("runtime.b.epoch"), RuntimePolicyId("policy.1"),
          RuntimePolicyRevision("1")}},
    RuntimeEpoch manager_epoch = RuntimeEpoch("manager.epoch.1"),
    RequestOwnerEpoch manager_owner = RequestOwnerEpoch("manager.owner.1")) {
  ManagerLivenessRequiredSetDTO result{CellId("cell.1"),         RetainedBundleId("bundle.1"),
                                       ContentDigest(kDigest),   RuntimeUnitId("manager"),
                                       std::move(manager_epoch), std::move(manager_owner),
                                       std::move(receivers)};
  result.required_runtime_set_digest = ComputeRequiredRuntimeSetDigest(result.required_receivers);
  return result;
}

ManagerLivenessObservationDTO LivenessObservation(const ManagerLivenessRequiredSetDTO& set,
                                                  std::size_t receiver, std::uint64_t sequence,
                                                  ManagerLivenessStateDTO state,
                                                  bool fenced = false) {
  ManagerLivenessStatusDTO status{set.cell_id,
                                  set.required_receivers.at(receiver).runtime_unit_id,
                                  set.required_receivers.at(receiver).runtime_epoch,
                                  set.manager_runtime_unit_id,
                                  set.manager_runtime_epoch,
                                  set.manager_owner_epoch,
                                  set.required_receivers.at(receiver).runtime_policy_id,
                                  set.required_receivers.at(receiver).runtime_policy_revision,
                                  sequence,
                                  sequence,
                                  0,
                                  state,
                                  fenced,
                                  std::nullopt};
  return {status, set.active_bundle_id, set.required_runtime_set_digest,
          ComputeManagerLivenessStatusDigest(status)};
}

KernelCellAdmissionOwner LivenessOwner(FixedClock& clock, AdmissionJournal& journal,
                                       const ManagerLivenessRequiredSetDTO& set) {
  return KernelCellAdmissionOwner(
      RequestOwnerEpoch("kernel.1"), 16U, clock, journal, {}, AdmissionGeneration(0U),
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&, const auto& reconcile) {
        return Result<CellAdmissionReconcileValidationDTO>::Ok(
            {reconcile.current_owner_epochs, {}});
      },
      {}, {}, set);
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

  auto acquiring = ready;
  acquiring.program_cycle_status = ProgramCycleStatusDTO{
      RequestOwnerEpoch("program.owner"), 2U,           ProgramCycleStateDTO::kStarting,
      ProgramRunId("program-cycle.2"),    std::nullopt, std::nullopt};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(acquiring).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  acquiring.program_cycle_status->state = ProgramCycleStateDTO::kStopping;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(acquiring).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  acquiring.program_cycle_status->state = ProgramCycleStateDTO::kResetting;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(acquiring).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  acquiring.program_cycle_status->state =
      ProgramCycleStateDTO::kRecoveryRequired;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(acquiring).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());

  for (int row = 0; row != 11; ++row) {
    auto blocked = ready;
    switch (row) {
      case 0:
        blocked.program_cycle_status_current = false;
        break;
      case 1:
        blocked.program_cycle_status =
            ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"),
                                  2U,
                                  ProgramCycleStateDTO::kTerminalAwaitingReset,
                                  ProgramRunId("run.1"),
                                  CellAdmissionClaimGeneration(1U),
                                  ContentDigest(kDigest)};
        break;
      case 2:
        blocked.required_component_status_current = false;
        break;
      case 3:
        blocked.required_component_total = 3U;
        break;
      case 4:
        blocked.required_component_current_compatible_ready_converged = 1U;
        break;
      case 5:
        blocked.deployment_status_current = false;
        break;
      case 6:
        blocked.deployment_converged = false;
        break;
      case 7:
        blocked.blocking_fault_count = 1U;
        break;
      case 8:
        blocked.unresolved_owner_count = 1U;
        break;
      case 9:
        blocked.unsettled_effect_count = 1U;
        break;
      case 10:
        blocked.program_cycle_status =
            ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 2U,
                                  ProgramCycleStateDTO::kRunning,     ProgramRunId("run.1"),
                                  CellAdmissionClaimGeneration(1U),   std::nullopt};
        break;
    }
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(blocked).ok());
    EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed) << row;
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  }
}

TEST(KernelCellAdmissionOwner,
     ProgramResetSceneAuthorizationRequiresExactResettingCycleAndClaim) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                       owner.GetAuthoringAdmission().admission_generation,
                       "program-reset.claim");
  intent.owner_epoch = RequestOwnerEpoch("program.owner");
  intent.owner_transaction_id = "program-cycle.2";
  const auto acquired = owner.AcquireCellAdmissionClaim(intent);
  ASSERT_TRUE(acquired.ok());
  const ProgramResetSceneAuthorizationDTO authorization{
      RequestOwnerEpoch("program.owner"), ProgramRunId("program-cycle.2"),
      acquired.value().claim_generation, ContentDigest(kDigest)};

  EXPECT_EQ(owner.ValidateProgramResetSceneAuthorization(authorization)
                .error()
                .details.at("reason"),
            "program_reset_status_mismatch");

  auto resetting = Ready();
  resetting.program_cycle_status = ProgramCycleStatusDTO{
      RequestOwnerEpoch("program.owner"), 2U,
      ProgramCycleStateDTO::kResetting, ProgramRunId("program-cycle.2"),
      acquired.value().claim_generation, ContentDigest(kDigest)};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(resetting).ok());
  EXPECT_TRUE(
      owner.ValidateProgramResetSceneAuthorization(authorization).ok());

  auto stale = authorization;
  stale.terminal_or_settlement_reference = ContentDigest(kDigestB);
  EXPECT_EQ(owner.ValidateProgramResetSceneAuthorization(stale)
                .error()
                .details.at("reason"),
            "program_reset_status_mismatch");
}

TEST(KernelCellAdmissionOwner, RestartContinuesTheDurableClaimGenerationHighWater) {
  FixedClock clock;
  AdmissionJournal journal;
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.restarted"), 16U, clock, journal, {}, AdmissionGeneration(40U),
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&) { return Result<void>::Ok(); }, {}, {}, {}, EmptyLivenessSet(),
      CellAdmissionClaimGeneration(29U));
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                       owner.GetAuthoringAdmission().admission_generation);

  const auto acquired = owner.AcquireCellAdmissionClaim(intent);

  ASSERT_TRUE(acquired.ok());
  EXPECT_EQ(acquired.value().claim_generation, CellAdmissionClaimGeneration(30U));
}

TEST(KernelCellAdmissionOwner, UnconfiguredManagerLivenessNeverOpensAuthoring) {
  FixedClock clock;
  AdmissionJournal journal;
  KernelCellAdmissionOwner owner(RequestOwnerEpoch("kernel.1"), 16U, clock, journal, {});
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
}

TEST(KernelCellAdmissionOwner, ManagerLivenessUsesExactBoundedSetAndAllCurrentUnfencedPredicate) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto set = LivenessSet();
  auto owner = LivenessOwner(clock, journal, set);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);

  const auto generation = owner.GetAuthoringAdmission().admission_generation;
  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 1U, ManagerLivenessStateDTO::kCurrent)).ok());
  EXPECT_GT(owner.GetAuthoringAdmission().admission_generation, generation);
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 1U, 1U, ManagerLivenessStateDTO::kCurrent)).ok());
  EXPECT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);

  EXPECT_FALSE(
      owner.Publish(LivenessObservation(set, 1U, 1U, ManagerLivenessStateDTO::kCurrent)).ok());
  auto fenced = LivenessObservation(set, 1U, 2U, ManagerLivenessStateDTO::kCurrent, true);
  ASSERT_TRUE(owner.Publish(fenced).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  auto expired = LivenessObservation(set, 1U, 3U, ManagerLivenessStateDTO::kExpired, true);
  ASSERT_TRUE(owner.Publish(expired).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  auto unreconciled =
      LivenessObservation(set, 1U, 4U, ManagerLivenessStateDTO::kUnreconciled, true);
  ASSERT_TRUE(owner.Publish(unreconciled).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
}

TEST(KernelCellAdmissionOwner,
     HealthyRenewalsDoNotChurnGenerationAndPredicateTransitionsAdvanceOnce) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto set = LivenessSet({{RuntimeUnitId("runtime.a"), RuntimeEpoch("runtime.a.epoch"),
                                 RuntimePolicyId("policy.1"), RuntimePolicyRevision("1")}});
  auto owner = LivenessOwner(clock, journal, set);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());

  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 1U, ManagerLivenessStateDTO::kCurrent)).ok());
  const auto current_generation = owner.GetAuthoringAdmission().admission_generation;
  const auto current_journal_sequence = journal.sequence.load();
  for (std::uint64_t sequence = 2U; sequence != 102U; ++sequence) {
    ASSERT_TRUE(
        owner.Publish(LivenessObservation(set, 0U, sequence, ManagerLivenessStateDTO::kCurrent))
            .ok());
  }
  EXPECT_EQ(owner.GetAuthoringAdmission().admission_generation, current_generation);
  EXPECT_EQ(journal.sequence.load(), current_journal_sequence);

  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 102U, ManagerLivenessStateDTO::kExpired, true))
          .ok());
  const auto expired_generation = owner.GetAuthoringAdmission().admission_generation;
  const auto expired_journal_sequence = journal.sequence.load();
  EXPECT_EQ(expired_generation.value(), current_generation.value() + 1U);
  EXPECT_EQ(expired_journal_sequence, current_journal_sequence + 1U);
  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 103U, ManagerLivenessStateDTO::kExpired, true))
          .ok());
  EXPECT_EQ(owner.GetAuthoringAdmission().admission_generation, expired_generation);
  EXPECT_EQ(journal.sequence.load(), expired_journal_sequence);

  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 104U, ManagerLivenessStateDTO::kCurrent)).ok());
  EXPECT_EQ(owner.GetAuthoringAdmission().admission_generation.value(),
            expired_generation.value() + 1U);
  EXPECT_EQ(journal.sequence.load(), expired_journal_sequence + 1U);
}

TEST(KernelCellAdmissionOwner, ManagerLivenessRejectsForeignFactsAndReplacementClearsRows) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto set = LivenessSet();
  auto owner = LivenessOwner(clock, journal, set);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto foreign = LivenessObservation(set, 0U, 1U, ManagerLivenessStateDTO::kCurrent);
  foreign.active_bundle_id = RetainedBundleId("foreign");
  foreign.receiver_status_digest = ComputeManagerLivenessStatusDigest(foreign.status);
  EXPECT_FALSE(owner.Publish(foreign).ok());
  foreign = LivenessObservation(set, 0U, 1U, ManagerLivenessStateDTO::kCurrent);
  foreign.required_runtime_set_digest = Digest('f');
  EXPECT_FALSE(owner.Publish(foreign).ok());
  auto wrong_epoch = LivenessObservation(set, 0U, 1U, ManagerLivenessStateDTO::kCurrent);
  wrong_epoch.status.receiver_runtime_epoch = RuntimeEpoch("foreign");
  wrong_epoch.receiver_status_digest = ComputeManagerLivenessStatusDigest(wrong_epoch.status);
  EXPECT_FALSE(owner.Publish(wrong_epoch).ok());
  auto wrong_policy = LivenessObservation(set, 0U, 1U, ManagerLivenessStateDTO::kCurrent);
  wrong_policy.status.runtime_policy_revision = RuntimePolicyRevision("2");
  wrong_policy.receiver_status_digest = ComputeManagerLivenessStatusDigest(wrong_policy.status);
  EXPECT_FALSE(owner.Publish(wrong_policy).ok());

  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 1U, ManagerLivenessStateDTO::kCurrent)).ok());
  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 1U, 1U, ManagerLivenessStateDTO::kCurrent)).ok());
  ASSERT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);
  auto replacement = LivenessSet(set.required_receivers, RuntimeEpoch("manager.epoch.2"),
                                 RequestOwnerEpoch("manager.owner.2"));
  const auto before = owner.GetAuthoringAdmission().admission_generation;
  const auto replaced = owner.ReplaceManagerLivenessRequiredSet(replacement);
  ASSERT_TRUE(replaced.ok());
  EXPECT_FALSE(replaced.value().authoring_allowed);
  EXPECT_GT(replaced.value().admission_generation, before);
  EXPECT_FALSE(
      owner.Publish(LivenessObservation(set, 0U, 2U, ManagerLivenessStateDTO::kCurrent)).ok());
}

TEST(KernelCellAdmissionOwner, RequiredRuntimeDigestRotatesForEpochAndPolicyOnlyReplacement) {
  const auto original = LivenessSet().required_receivers;
  auto epoch_replaced = original;
  epoch_replaced.front().runtime_epoch = RuntimeEpoch("runtime.a.epoch.2");
  auto policy_replaced = original;
  policy_replaced.front().runtime_policy_revision = RuntimePolicyRevision("2");

  EXPECT_NE(ComputeRequiredRuntimeSetDigest(original),
            ComputeRequiredRuntimeSetDigest(epoch_replaced));
  EXPECT_NE(ComputeRequiredRuntimeSetDigest(original),
            ComputeRequiredRuntimeSetDigest(policy_replaced));
}

TEST(KernelCellAdmissionOwner, DirectManagerLossImmediatelyInhibitsAndIsSequenceIdempotent) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto set = LivenessSet({{RuntimeUnitId("runtime.a"), RuntimeEpoch("runtime.a.epoch"),
                                 RuntimePolicyId("policy.1"), RuntimePolicyRevision("1")}});
  auto owner = LivenessOwner(clock, journal, set);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 1U, ManagerLivenessStateDTO::kCurrent)).ok());
  ASSERT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);
  const ManagerLivenessLossIntentDTO loss{set.cell_id,
                                          set.active_bundle_id,
                                          set.required_runtime_set_digest,
                                          set.required_receivers.front().runtime_unit_id,
                                          set.required_receivers.front().runtime_epoch,
                                          set.required_receivers.front().runtime_policy_id,
                                          set.required_receivers.front().runtime_policy_revision,
                                          set.manager_runtime_unit_id,
                                          set.manager_runtime_epoch,
                                          set.manager_owner_epoch,
                                          2U,
                                          ManagerLivenessLossKindDTO::kExpiry};

  EXPECT_TRUE(owner.LatchManagerLivenessLoss(loss));
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  EXPECT_FALSE(owner.LatchManagerLivenessLoss(loss));
  auto stale_unfence = LivenessObservation(set, 0U, 2U, ManagerLivenessStateDTO::kCurrent);
  EXPECT_FALSE(owner.Publish(stale_unfence).ok());
  auto fenced = LivenessObservation(set, 0U, 2U, ManagerLivenessStateDTO::kExpired, true);
  EXPECT_TRUE(owner.Publish(fenced).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
}

TEST(KernelCellAdmissionOwner, EmptyConfiguredManagerLivenessSetIsComplete) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto set = LivenessSet({});
  auto owner = LivenessOwner(clock, journal, set);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  EXPECT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);
}

TEST(KernelCellAdmissionOwner, ManagerLivenessRequiredSetIsBoundedAndCanonical) {
  FixedClock clock;
  AdmissionJournal journal;
  auto duplicate = LivenessSet({{RuntimeUnitId("runtime.a"), RuntimeEpoch("epoch.1"),
                                 RuntimePolicyId("policy.1"), RuntimePolicyRevision("1")},
                                {RuntimeUnitId("runtime.a"), RuntimeEpoch("epoch.2"),
                                 RuntimePolicyId("policy.1"), RuntimePolicyRevision("1")}});
  EXPECT_THROW(LivenessOwner(clock, journal, duplicate), std::invalid_argument);
  auto unsorted = LivenessSet({{RuntimeUnitId("runtime.b"), RuntimeEpoch("epoch.1"),
                                RuntimePolicyId("policy.1"), RuntimePolicyRevision("1")},
                               {RuntimeUnitId("runtime.a"), RuntimeEpoch("epoch.2"),
                                RuntimePolicyId("policy.1"), RuntimePolicyRevision("1")}});
  EXPECT_THROW(LivenessOwner(clock, journal, unsorted), std::invalid_argument);
}

TEST(KernelCellAdmissionOwner, ClosedFreshMatrixAndReplayReleaseFences) {
  constexpr CellAdmissionClaimKindDTO kinds[] = {
      CellAdmissionClaimKindDTO::kProgramCycle,  CellAdmissionClaimKindDTO::kAuthoringMutation,
      CellAdmissionClaimKindDTO::kManagerApply,  CellAdmissionClaimKindDTO::kComponentLifecycle,
      CellAdmissionClaimKindDTO::kAuthoredScene, CellAdmissionClaimKindDTO::kManualOperation};
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
    EXPECT_EQ(owner
                  .AcquireCellAdmissionClaim(
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
                    .AcquireCellAdmissionClaim(
                        Intent(CellAdmissionClaimKindDTO::kInterventionOrRecovery,
                               standalone.GetAuthoringAdmission().admission_generation))
                    .ok());
  }
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto cycle =
      owner.AcquireCellAdmissionClaim(Intent(CellAdmissionClaimKindDTO::kProgramCycle,
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
  ASSERT_TRUE(
      conflict
          .AcquireCellAdmissionClaim(Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                                            conflict.GetAuthoringAdmission().admission_generation))
          .ok());
  EXPECT_EQ(
      conflict
          .AcquireCellAdmissionClaim(Intent(CellAdmissionClaimKindDTO::kInterventionOrRecovery,
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
  auto claim =
      owner.AcquireCellAdmissionClaim(Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                                             owner.GetAuthoringAdmission().admission_generation));
  ASSERT_TRUE(claim.ok());
  auto stale_intent = Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, AdmissionGeneration(1U),
                             "request.stale");
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(stale_intent).error().code, "stale_generation");
  EXPECT_EQ(owner
                .AcquireCellAdmissionClaim(Intent(
                    CellAdmissionClaimKindDTO::kAuthoringMutation,
                    owner.GetAuthoringAdmission().admission_generation, "request.while-held"))
                .error()
                .code,
            "request_ledger_full");
  auto foreign = Release(claim.value());
  foreign.owner_epoch = RequestOwnerEpoch("foreign.owner");
  EXPECT_EQ(owner.ReleaseCellAdmissionClaim(foreign).error().code, "stale_generation");
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(claim.value())).ok());
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(claim.value().intent).error().code,
            "request_result_expired");
  auto conflicting = claim.value().intent;
  conflicting.owner_transaction_id = "conflicting.transaction";
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(conflicting).error().code, "request_id_conflict");
  const auto released = owner.GetCellAdmissionRecoveryReceipt(
      {claim.value().intent, claim.value(), Release(claim.value())});
  ASSERT_TRUE(released.ok());
  EXPECT_EQ(released.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);
  auto successor = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
             owner.GetAuthoringAdmission().admission_generation, "request.2"));
  ASSERT_TRUE(successor.ok());
  const auto retired = owner.GetCellAdmissionRecoveryReceipt(
      {claim.value().intent, claim.value(), Release(claim.value())});
  ASSERT_TRUE(retired.ok());
  EXPECT_EQ(retired.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
  const auto held = owner.GetCellAdmissionRecoveryReceipt(
      {successor.value().intent, successor.value(), std::nullopt});
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
}

TEST(KernelCellAdmissionOwner, CapacityPlusOneRepeatedCyclesRetireOnlyAfterSuccessorAcceptance) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal, 2U);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  std::optional<CellAdmissionClaimDTO> predecessor;
  for (std::size_t index = 0; index != 3U; ++index) {
    auto claim = owner.AcquireCellAdmissionClaim(Intent(
        CellAdmissionClaimKindDTO::kProgramCycle,
        owner.GetAuthoringAdmission().admission_generation, "cycle." + std::to_string(index)));
    ASSERT_TRUE(claim.ok());
    if (predecessor.has_value()) {
      const auto old = owner.GetCellAdmissionRecoveryReceipt(
          {predecessor->intent, *predecessor, Release(*predecessor)});
      ASSERT_TRUE(old.ok());
      EXPECT_EQ(old.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
    }
    const auto release = Release(claim.value());
    ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(release).ok());
    const auto exact =
        owner.GetCellAdmissionRecoveryReceipt({claim.value().intent, claim.value(), release});
    ASSERT_TRUE(exact.ok());
    EXPECT_EQ(exact.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);
    predecessor = claim.value();
  }
}

TEST(KernelCellAdmissionOwner,
     RecoveryReceiptDistinguishesUnknownConflictHeldAndReleasedWithoutMutation) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const auto unknown_intent =
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
             owner.GetAuthoringAdmission().admission_generation, "request.unknown");
  const auto before_unknown = journal.sequence.load();
  const auto unknown =
      owner.GetCellAdmissionRecoveryReceipt({unknown_intent, std::nullopt, std::nullopt});
  ASSERT_TRUE(unknown.ok());
  EXPECT_EQ(unknown.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
  EXPECT_EQ(journal.sequence.load(), before_unknown);

  auto claim =
      owner.AcquireCellAdmissionClaim(Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                                             owner.GetAuthoringAdmission().admission_generation));
  ASSERT_TRUE(claim.ok());
  const auto after_acquire = journal.sequence.load();
  const auto held =
      owner.GetCellAdmissionRecoveryReceipt({claim.value().intent, std::nullopt, std::nullopt});
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  EXPECT_EQ(held.value().matched_claim, claim.value());
  EXPECT_EQ(held.value().held_claims, std::vector<CellAdmissionClaimDTO>{claim.value()});
  const std::vector<CellAdmissionCurrentOwnerEpochDTO> expected_current_owners{
      {claim.value().intent.owner_role, claim.value().intent.owner_epoch}};
  EXPECT_EQ(held.value().current_owner_epochs, expected_current_owners);
  EXPECT_EQ(journal.sequence.load(), after_acquire);

  auto conflicting_intent = claim.value().intent;
  conflicting_intent.owner_transaction_id = "transaction.conflict";
  const auto conflicting =
      owner.GetCellAdmissionRecoveryReceipt({conflicting_intent, std::nullopt, std::nullopt});
  ASSERT_TRUE(conflicting.ok());
  EXPECT_EQ(conflicting.value().status, CellAdmissionRecoveryReceiptStatusDTO::kConflicting);
  EXPECT_FALSE(conflicting.value().matched_claim.has_value());

  const auto release = Release(claim.value());
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(release).ok());
  const auto after_release = journal.sequence.load();
  const auto released =
      owner.GetCellAdmissionRecoveryReceipt({claim.value().intent, claim.value(), release});
  ASSERT_TRUE(released.ok());
  EXPECT_EQ(released.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);
  EXPECT_EQ(released.value().matched_release, release);
  EXPECT_TRUE(released.value().held_claims.empty());
  EXPECT_EQ(journal.sequence.load(), after_release);
  EXPECT_EQ(owner.ReleaseCellAdmissionClaim(release).error().code, "stale_generation");
}

TEST(KernelCellAdmissionOwner, ExactCompactedMembershipIsUnknownButIdentityMismatchIsConflicting) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto intent =
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, AdmissionGeneration(3U));
  const auto digest = CellAdmissionClaimIntentDigest(intent);
  ASSERT_TRUE(digest.ok());
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.2"), 2U, clock, journal, {}, AdmissionGeneration(3U), {}, {}, {},
      {{intent.owner_role, intent.owner_epoch, intent.request_id, digest.value()}});

  const auto exact = owner.GetCellAdmissionRecoveryReceipt({intent, std::nullopt, std::nullopt});
  ASSERT_TRUE(exact.ok());
  EXPECT_EQ(exact.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);

  auto mismatch = intent;
  mismatch.owner_transaction_id = "transaction.mismatch";
  const auto conflicting =
      owner.GetCellAdmissionRecoveryReceipt({mismatch, std::nullopt, std::nullopt});
  ASSERT_TRUE(conflicting.ok());
  EXPECT_EQ(conflicting.value().status, CellAdmissionRecoveryReceiptStatusDTO::kConflicting);
}

TEST(KernelCellAdmissionOwner,
     RestartRetainsLatestReconciledReceiptUntilExactSuccessorProgression) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto first_intent =
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, AdmissionGeneration(7U));
  const CellAdmissionClaimDTO first{first_intent, CellAdmissionClaimGeneration(4U),
                                    AdmissionGeneration(7U), 41};
  auto second_intent =
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, AdmissionGeneration(8U), "request.2");
  second_intent.owner_epoch = RequestOwnerEpoch("owner.2");
  second_intent.owner_transaction_id = "transaction.2";
  const CellAdmissionClaimDTO second{second_intent, CellAdmissionClaimGeneration(5U),
                                     AdmissionGeneration(8U), 42};
  const CellAdmissionReconcileSettlementDTO first_settlement{
      first, CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.2"),
      CellAdmissionSettlementKindDTO::kReconciled, ContentDigest(kDigest)};
  KernelCellAdmissionOwner restarted(
      RequestOwnerEpoch("kernel.3"), 2U, clock, journal,
      {{first, std::nullopt, first_settlement}, {second, std::nullopt, std::nullopt}},
      AdmissionGeneration(9U), {}, {},
      [&](const auto& held, const auto&, const auto& reconcile) {
        return Result<CellAdmissionReconcileValidationDTO>::Ok(
            {reconcile.current_owner_epochs,
             {{held.front(), CellAdmissionOwnerRoleDTO::kAuthoringOwner,
               RequestOwnerEpoch("owner.3"), CellAdmissionSettlementKindDTO::kReconciled,
               reconcile.reconciliation_evidence_digest}}});
      },
      {}, {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.2")}});

  const auto recovered =
      restarted.GetCellAdmissionRecoveryReceipt({first.intent, first, std::nullopt});
  ASSERT_TRUE(recovered.ok());
  EXPECT_EQ(recovered.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact);
  EXPECT_EQ(recovered.value().held_claims, std::vector<CellAdmissionClaimDTO>{second});

  const CellAdmissionReconcileDTO successor{
      RequestOwnerEpoch("kernel.3"),
      {second},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.3")}},
      ContentDigest(kDigestB)};
  ASSERT_TRUE(restarted.ReconcileCellAdmissionClaims(successor).ok());
  const auto retired =
      restarted.GetCellAdmissionRecoveryReceipt({first.intent, first, std::nullopt});
  ASSERT_TRUE(retired.ok());
  EXPECT_EQ(retired.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
  const auto latest =
      restarted.GetCellAdmissionRecoveryReceipt({second.intent, second, std::nullopt});
  ASSERT_TRUE(latest.ok());
  EXPECT_EQ(latest.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact);
}

TEST(KernelCellAdmissionOwner,
     RepeatedOwnerReplacementKeepsMembershipBoundedAndNeverCompactsHeldClaim) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = KernelCellAdmissionOwner(
      RequestOwnerEpoch("kernel.1"), 2U, clock, journal, {}, AdmissionGeneration(1U), {},
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto& held, const auto&, const auto& reconcile) {
        return Result<CellAdmissionReconcileValidationDTO>::Ok(
            {reconcile.current_owner_epochs,
             {{held.front(), CellAdmissionOwnerRoleDTO::kAuthoringOwner,
               reconcile.current_owner_epochs.front().owner_epoch,
               CellAdmissionSettlementKindDTO::kReconciled,
               reconcile.reconciliation_evidence_digest}}});
      },
      {}, {}, EmptyLivenessSet());
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto acquired =
      owner.AcquireCellAdmissionClaim(Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                                             owner.GetAuthoringAdmission().admission_generation));
  ASSERT_TRUE(acquired.ok());
  auto predecessor = acquired.value();
  std::optional<CellAdmissionClaimDTO> obsolete;

  for (std::uint64_t index = 2U; index <= 8U; ++index) {
    const RequestOwnerEpoch successor_epoch("owner." + std::to_string(index));
    const CellAdmissionReconcileDTO reconcile{
        RequestOwnerEpoch("kernel.1"),
        {predecessor},
        {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, successor_epoch}},
        Digest(static_cast<char>('0' + index))};
    ASSERT_TRUE(owner.ReconcileCellAdmissionClaims(reconcile).ok()) << index;
    if (obsolete.has_value()) {
      const auto retired =
          owner.GetCellAdmissionRecoveryReceipt({obsolete->intent, *obsolete, std::nullopt});
      ASSERT_TRUE(retired.ok());
      EXPECT_EQ(retired.value().status, CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
    }
    const auto latest =
        owner.GetCellAdmissionRecoveryReceipt({predecessor.intent, predecessor, std::nullopt});
    ASSERT_TRUE(latest.ok());
    EXPECT_EQ(latest.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact);

    auto next_intent = Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                              owner.GetAuthoringAdmission().admission_generation,
                              "request." + std::to_string(index));
    next_intent.owner_epoch = successor_epoch;
    next_intent.owner_transaction_id = "transaction." + std::to_string(index);
    auto next = owner.AcquireCellAdmissionClaim(next_intent);
    ASSERT_TRUE(next.ok()) << index;
    const auto held =
        owner.GetCellAdmissionRecoveryReceipt({next.value().intent, next.value(), std::nullopt});
    ASSERT_TRUE(held.ok());
    EXPECT_EQ(held.value().status, CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
    EXPECT_EQ(held.value().held_claims, std::vector<CellAdmissionClaimDTO>{next.value()});
    obsolete = predecessor;
    predecessor = next.value();
  }
}

TEST(KernelCellAdmissionOwner, ReleaseUsesImmutableAcquireGenerationNotCurrentGeneration) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto claim =
      owner.AcquireCellAdmissionClaim(Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                                             owner.GetAuthoringAdmission().admission_generation));
  ASSERT_TRUE(claim.ok());
  auto changed = Ready();
  changed.deployment_converged = false;
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
  const CellAdmissionClaimDTO first{first_intent, CellAdmissionClaimGeneration(4U),
                                    AdmissionGeneration(7U), 41};
  const CellAdmissionClaimDTO second{second_intent, CellAdmissionClaimGeneration(5U),
                                     AdmissionGeneration(8U), 42};
  const std::vector<OperationJournalRecoveryDTO::RecoveredCellAdmissionClaimDTO> recovery{
      {first, std::nullopt, std::nullopt}, {second, std::nullopt, std::nullopt}};
  const std::vector<CellAdmissionCurrentOwnerEpochDTO> successors{
      {CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.2")},
      {CellAdmissionOwnerRoleDTO::kManager, RequestOwnerEpoch("manager.owner.1")}};
  const auto make_owner = [&](bool authorize_first) {
    return KernelCellAdmissionOwner(
        RequestOwnerEpoch("kernel.2"), 4U, clock, journal, recovery, AdmissionGeneration(8U), {},
        [](const auto&, const auto&) { return Result<void>::Ok(); },
        [&, authorize_first](const auto& held, const auto&, const auto& reconcile) {
          CellAdmissionReconcileValidationDTO validation{reconcile.current_owner_epochs, {}};
          if (authorize_first) {
            validation.settlements.push_back(
                {held.front(), CellAdmissionOwnerRoleDTO::kAuthoringOwner,
                 RequestOwnerEpoch("owner.2"), CellAdmissionSettlementKindDTO::kReconciled,
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
  const auto rejected_bytes = byte_different.ReconcileCellAdmissionClaims(changed_inventory);
  ASSERT_FALSE(rejected_bytes.ok());
  EXPECT_EQ(rejected_bytes.error().details.at("reason"), "reconcile_inventory_not_exact");

  auto incomplete = make_owner(false);
  auto omitted = reconcile;
  omitted.retained_claims.pop_back();
  EXPECT_EQ(incomplete.ReconcileCellAdmissionClaims(omitted).error().details.at("reason"),
            "reconcile_inventory_not_exact");

  auto raw_successor_only = make_owner(false);
  const auto raw_rotation = raw_successor_only.ReconcileCellAdmissionClaims(reconcile);
  ASSERT_FALSE(raw_rotation.ok());
  EXPECT_EQ(raw_rotation.error().details.at("reason"), "reconcile_predecessor_unsettled");
  EXPECT_TRUE(raw_successor_only.has_unresolved_claims());

  auto stale_release = KernelCellAdmissionOwner(
      RequestOwnerEpoch("kernel.2"), 4U, clock, journal, recovery, AdmissionGeneration(8U), {},
      [](const auto&, const auto&) { return Result<void>::Ok(); }, {}, {}, successors);
  EXPECT_EQ(stale_release.ReleaseCellAdmissionClaim(Release(first)).error().code,
            "stale_generation");

  auto settled = make_owner(true);
  ASSERT_TRUE(settled.ReconcileCellAdmissionClaims(reconcile).ok());
  EXPECT_TRUE(settled.has_unresolved_claims());
  const auto receipt = settled.GetCellAdmissionRecoveryReceipt({first.intent, first, std::nullopt});
  ASSERT_TRUE(receipt.ok());
  EXPECT_EQ(receipt.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact);
  ASSERT_TRUE(receipt.value().matched_reconcile_settlement.has_value());
  EXPECT_EQ(receipt.value().matched_reconcile_settlement->predecessor_claim, first);
  EXPECT_EQ(receipt.value().held_claims, std::vector<CellAdmissionClaimDTO>{second});
  EXPECT_EQ(receipt.value().current_owner_epochs, successors);
  EXPECT_TRUE(settled.ReconcileCellAdmissionClaims(reconcile).ok());
  EXPECT_EQ(settled.ReleaseCellAdmissionClaim(Release(first)).error().code, "stale_generation");
  EXPECT_TRUE(settled.ReleaseCellAdmissionClaim(Release(second)).ok());
}

TEST(KernelCellAdmissionOwner, SupportsMultipleExactReconciliationsInOneKernelEpoch) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto intent =
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, AdmissionGeneration(7U));
  const CellAdmissionClaimDTO claim{intent, CellAdmissionClaimGeneration(4U),
                                    AdmissionGeneration(7U), 41};
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.2"), 4U, clock, journal, {{claim, std::nullopt, std::nullopt}},
      AdmissionGeneration(8U), {}, {},
      [&](const auto& held, const auto&, const auto& reconcile) {
        CellAdmissionReconcileValidationDTO validation{reconcile.current_owner_epochs, {}};
        if (reconcile.reconciliation_evidence_digest == ContentDigest(kDigestB)) {
          validation.settlements.push_back(
              {held.front(), CellAdmissionOwnerRoleDTO::kAuthoringOwner,
               RequestOwnerEpoch("owner.2"), CellAdmissionSettlementKindDTO::kReconciled,
               reconcile.reconciliation_evidence_digest});
        }
        return Result<CellAdmissionReconcileValidationDTO>::Ok(validation);
      },
      {}, {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.1")}});

  const CellAdmissionReconcileDTO first{
      RequestOwnerEpoch("kernel.2"),
      {claim},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.1")}},
      ContentDigest(kDigest)};
  ASSERT_TRUE(owner.ReconcileCellAdmissionClaims(first).ok());
  ASSERT_TRUE(owner.ReconcileCellAdmissionClaims(first).ok());

  const CellAdmissionReconcileDTO second{
      RequestOwnerEpoch("kernel.2"),
      {claim},
      {{CellAdmissionOwnerRoleDTO::kAuthoringOwner, RequestOwnerEpoch("owner.2")}},
      ContentDigest(kDigestB)};
  ASSERT_TRUE(owner.ReconcileCellAdmissionClaims(second).ok());
  ASSERT_TRUE(owner.ReconcileCellAdmissionClaims(second).ok());
  EXPECT_FALSE(owner.has_unresolved_claims());
  EXPECT_EQ(owner.ReleaseCellAdmissionClaim(Release(claim)).error().code, "stale_generation");
}

TEST(KernelCellAdmissionOwner, ScopeIdentitiesAreBoundedBeforeRecoveryFootprintCalculation) {
  auto intent = Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, AdmissionGeneration(1U));
  intent.scope.resources = {ResourceId(std::string(kMaximumCellAdmissionIdentityBytes + 1U, 'r'))};
  EXPECT_FALSE(ValidateCellAdmissionClaimIntent(intent).ok());
  intent.scope.resources.clear();
  intent.scope.configurations = {CellAdmissionConfigurationScopeDTO{
      ControlDomainId(std::string(kMaximumCellAdmissionIdentityBytes + 1U, 'd')),
      ActiveControlConfigurationId("configuration"), ActiveControlConfigurationGeneration(1U)}};
  EXPECT_FALSE(ValidateCellAdmissionClaimIntent(intent).ok());
  intent.scope.configurations.front().control_domain_id = ControlDomainId("domain");
  intent.scope.configurations.front().active_configuration_id =
      ActiveControlConfigurationId(std::string(kMaximumCellAdmissionIdentityBytes + 1U, 'c'));
  EXPECT_FALSE(ValidateCellAdmissionClaimIntent(intent).ok());
}

TEST(KernelCellAdmissionOwner, RecoveryFootprintIsAggregateEncodingSafeAndRejectsRestartState) {
  for (const auto binary :
       {kMaximumCellAdmissionPrivateBinaryBytes - 1U, kMaximumCellAdmissionPrivateBinaryBytes,
        kMaximumCellAdmissionPrivateBinaryBytes + 1U}) {
    const CellAdmissionRecoveryFootprintDTO boundary{binary, binary, 1U, 1U};
    EXPECT_EQ(boundary.admitted(), binary <= kMaximumCellAdmissionPrivateBinaryBytes);
  }
  for (const auto json :
       {kMaximumCellAdmissionJsonRequestBytes - 1U, kMaximumCellAdmissionJsonRequestBytes,
        kMaximumCellAdmissionJsonRequestBytes + 1U}) {
    const CellAdmissionRecoveryFootprintDTO boundary{1U, 1U, json,
                                                     kMaximumCellAdmissionJsonResponseBytes};
    EXPECT_EQ(boundary.admitted(), json <= kMaximumCellAdmissionJsonRequestBytes);
  }
  for (const auto json :
       {kMaximumCellAdmissionJsonResponseBytes - 1U, kMaximumCellAdmissionJsonResponseBytes,
        kMaximumCellAdmissionJsonResponseBytes + 1U}) {
    const CellAdmissionRecoveryFootprintDTO boundary{1U, 1U, 1U, json};
    EXPECT_EQ(boundary.admitted(), json <= kMaximumCellAdmissionJsonResponseBytes);
  }

  auto make_claim = [](std::size_t resources, char fill, CellAdmissionClaimGeneration generation,
                       AdmissionGeneration admission) {
    auto intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle, admission,
                         "footprint." + std::to_string(generation.value()));
    for (std::size_t index = 0; index < resources; ++index) {
      auto number = std::to_string(index);
      auto prefix = "resource." + std::string(4U - number.size(), '0') + number;
      prefix.append(kMaximumCellAdmissionIdentityBytes - prefix.size(), fill);
      intent.scope.resources.emplace_back(std::move(prefix));
    }
    return CellAdmissionClaimDTO{std::move(intent), generation, admission, 42};
  };
  std::size_t first_rejected = 1U;
  for (; first_rejected <= kMaximumCellAdmissionScopeEntries; ++first_rejected) {
    const auto claim =
        make_claim(first_rejected, '"', CellAdmissionClaimGeneration(1U), AdmissionGeneration(1U));
    const auto footprint = ComputeCellAdmissionRecoveryFootprint(
        RequestOwnerEpoch("kernel.1"), AdmissionGeneration(2U),
        {{claim, std::nullopt, std::nullopt}},
        {{CellAdmissionOwnerRoleDTO::kProgramRunner, claim.intent.owner_epoch}});
    ASSERT_TRUE(footprint.ok());
    if (!footprint.value().admitted()) {
      break;
    }
  }
  ASSERT_GT(first_rejected, 1U);
  ASSERT_LE(first_rejected, kMaximumCellAdmissionScopeEntries);
  const auto boundary_minus_one = make_claim(
      first_rejected - 1U, '"', CellAdmissionClaimGeneration(1U), AdmissionGeneration(1U));
  const auto over =
      make_claim(first_rejected, '"', CellAdmissionClaimGeneration(1U), AdmissionGeneration(1U));
  const auto admitted = ComputeCellAdmissionRecoveryFootprint(
      RequestOwnerEpoch("kernel.1"), AdmissionGeneration(2U),
      {{boundary_minus_one, std::nullopt, std::nullopt}},
      {{CellAdmissionOwnerRoleDTO::kProgramRunner, boundary_minus_one.intent.owner_epoch}});
  const auto rejected = ComputeCellAdmissionRecoveryFootprint(
      RequestOwnerEpoch("kernel.1"), AdmissionGeneration(2U), {{over, std::nullopt, std::nullopt}},
      {{CellAdmissionOwnerRoleDTO::kProgramRunner, over.intent.owner_epoch}});
  ASSERT_TRUE(admitted.ok());
  ASSERT_TRUE(rejected.ok());
  EXPECT_TRUE(admitted.value().admitted());
  EXPECT_FALSE(rejected.value().admitted());

  FixedClock acquire_clock;
  AdmissionJournal acquire_journal;
  auto acquire_owner = Owner(acquire_clock, acquire_journal);
  ASSERT_TRUE(acquire_owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto over_intent = over.intent;
  over_intent.expected_admission_generation =
      acquire_owner.GetAuthoringAdmission().admission_generation;
  const auto sequence_before = acquire_journal.sequence.load();
  const auto oversized_acquire = acquire_owner.AcquireCellAdmissionClaim(over_intent);
  ASSERT_FALSE(oversized_acquire.ok());
  EXPECT_EQ(oversized_acquire.error().code, "invalid_request");
  EXPECT_EQ(oversized_acquire.error().details.at("reason"), "recovery_footprint_exceeded");
  EXPECT_EQ(acquire_journal.sequence.load(), sequence_before);

  auto intervention = Intent(CellAdmissionClaimKindDTO::kInterventionOrRecovery,
                             AdmissionGeneration(2U), "footprint.intervention");
  intervention.scope.resources = boundary_minus_one.intent.scope.resources;
  intervention.intervention_evidence->parent_program_cycle_claim_generation =
      boundary_minus_one.claim_generation;
  const CellAdmissionClaimDTO recovery_claim{intervention, CellAdmissionClaimGeneration(2U),
                                             AdmissionGeneration(2U), 43};
  const auto parent_and_recovery = ComputeCellAdmissionRecoveryFootprint(
      RequestOwnerEpoch("kernel.1"), AdmissionGeneration(3U),
      {{boundary_minus_one, std::nullopt, std::nullopt},
       {recovery_claim, std::nullopt, std::nullopt}},
      {{CellAdmissionOwnerRoleDTO::kProgramRunner, boundary_minus_one.intent.owner_epoch},
       {CellAdmissionOwnerRoleDTO::kInterventionOrRecoveryOwner,
        recovery_claim.intent.owner_epoch}});
  ASSERT_TRUE(parent_and_recovery.ok());
  EXPECT_FALSE(parent_and_recovery.value().admitted());

  const auto ordinary =
      make_claim(1U, 'x', CellAdmissionClaimGeneration(7U), AdmissionGeneration(7U));
  const auto released_form = ComputeCellAdmissionRecoveryFootprint(
      RequestOwnerEpoch("kernel.1"), AdmissionGeneration(8U),
      {{ordinary, Release(ordinary), std::nullopt}},
      {{CellAdmissionOwnerRoleDTO::kProgramRunner, ordinary.intent.owner_epoch}});
  const CellAdmissionReconcileSettlementDTO reconciliation{
      ordinary, CellAdmissionOwnerRoleDTO::kProgramRunner, RequestOwnerEpoch("program.successor"),
      CellAdmissionSettlementKindDTO::kReconciled, ContentDigest(kDigest)};
  const auto reconciled_form = ComputeCellAdmissionRecoveryFootprint(
      RequestOwnerEpoch("kernel.1"), AdmissionGeneration(8U),
      {{ordinary, std::nullopt, reconciliation}},
      {{CellAdmissionOwnerRoleDTO::kProgramRunner, RequestOwnerEpoch("program.successor")}});
  ASSERT_TRUE(released_form.ok());
  ASSERT_TRUE(reconciled_form.ok());
  EXPECT_TRUE(released_form.value().admitted());
  EXPECT_TRUE(reconciled_form.value().admitted());

  FixedClock clock;
  AdmissionJournal journal;
  EXPECT_THROW(
      KernelCellAdmissionOwner(RequestOwnerEpoch("kernel.restart"), 2U, clock, journal,
                               {{over, std::nullopt, std::nullopt}}, AdmissionGeneration(2U)),
      std::runtime_error);
}

}  // namespace
