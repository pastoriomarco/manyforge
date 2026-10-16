#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <future>
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
  Result<std::uint64_t> RecordOperationState(
      const RequestOwnerEpoch&, const OperationStateDTO&, const RuntimeScaleStatusDTO*, const char*,
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
    if (fail_generation) return Result<std::uint64_t>::Err(ErrorCode::kInternalError);
    return Result<std::uint64_t>::Ok(++sequence);
  }

  Result<CellAdmissionClaimDTO> RecordConfigurationApplyClaimAcquired(
      const RequestOwnerEpoch&, const CellAdmissionClaimDTO& claim,
      const CellAdmissionCurrentOwnerEpochDTO&, const std::vector<CellAdmissionClaimDTO>&,
      const std::vector<CellAdmissionCurrentOwnerEpochDTO>&) override {
    if (fail_configuration_write)
      return Result<CellAdmissionClaimDTO>::Err(ErrorCode::kInternalError);
    ++sequence;
    return Result<CellAdmissionClaimDTO>::Ok(claim);
  }
  Result<AdmissionGeneration> RecordTerminalApplyClaimsReleasedBySuccess(
      const RequestOwnerEpoch&, const CellAdmissionClaimDTO&, const CellAdmissionClaimReleaseDTO&,
      const std::vector<CellAdmissionClaimDTO>& held,
      const std::vector<CellAdmissionCurrentOwnerEpochDTO>&,
      AdmissionGeneration generation) override {
    if (fail_configuration_write)
      return Result<AdmissionGeneration>::Err(ErrorCode::kInternalError);
    ++sequence;
    return Result<AdmissionGeneration>::Ok(
        AdmissionGeneration(generation.value() + (held.size() > 1U)));
  }
  bool fail_configuration_write{};
  bool fail_generation{};
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

AcceptedProgramResetDTO AcceptedReset() {
  return {RequestOwnerEpoch("operator.1"), RequestId("reset.1"),
          ContentDigest(kDigest), true, false, std::nullopt};
}

CellAdmissionOwnerRoleDTO Role(CellAdmissionClaimKindDTO kind) {
  switch (kind) {
    case CellAdmissionClaimKindDTO::kProgramCycle:
    case CellAdmissionClaimKindDTO::kProgramReset:
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
      (claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle ||
       claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramReset)
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
                               std::size_t capacity = 16U,
                               CellAdmissionProgramNodeValidator program_node_validator = {},
                               std::function<bool()> stop_fault_active = {},
                               CellAdmissionProgramResetStatusSource reset_status_source = {}) {
  return KernelCellAdmissionOwner(
      RequestOwnerEpoch("kernel.1"), capacity, clock, journal, {}, AdmissionGeneration(0U),
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&, const auto& reconcile) {
        return Result<CellAdmissionReconcileValidationDTO>::Ok(
            {reconcile.current_owner_epochs, {}});
      },
      {}, {}, EmptyLivenessSet(), CellAdmissionClaimGeneration(0U),
      std::move(program_node_validator), std::move(stop_fault_active), std::move(reset_status_source));
}

TEST(KernelCellAdmissionOwner,
     StopFaultPreservesReplayAndAllResponsesStayClosedUntilPublishedClear) {
  FixedClock clock;
  AdmissionJournal journal;
  bool fault = false;
  auto owner = Owner(clock, journal, 16U, {}, [&] { return fault; });
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).value().authoring_allowed);
  const auto intent = Intent(CellAdmissionClaimKindDTO::kComponentLifecycle,
                             owner.GetAuthoringAdmission().admission_generation);
  const auto accepted = owner.AcquireCellAdmissionClaim(intent);
  ASSERT_TRUE(accepted.ok());
  fault = true;
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(intent).value(), accepted.value());
  EXPECT_FALSE(owner.UpdateCellAdmissionReadiness(Ready()).value().authoring_allowed);
  EXPECT_FALSE(
      owner.ReleaseCellAdmissionClaim(Release(accepted.value())).value().authoring_allowed);
  auto fresh = Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                      owner.GetAuthoringAdmission().admission_generation, "fresh");
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(fresh).error().details.at("reason"),
            "active_stop_fault_requires_recovery");
  const auto before = owner.GetAuthoringAdmission().admission_generation;
  ASSERT_TRUE(owner.RefreshStopFaultAdmission().ok());
  EXPECT_GT(owner.GetAuthoringAdmission().admission_generation.value(), before.value());
  fault = false;
  journal.fail_generation = true;
  EXPECT_FALSE(owner.RefreshStopFaultAdmission().ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  EXPECT_FALSE(owner.UpdateCellAdmissionReadiness(Ready()).value().authoring_allowed);
  journal.fail_generation = false;
  ASSERT_TRUE(owner.RefreshStopFaultAdmission().ok());
  EXPECT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(fresh).ok());  // pre-clear generation
  fresh.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  EXPECT_TRUE(owner.AcquireCellAdmissionClaim(fresh).ok());
}

TEST(KernelCellAdmissionOwner, ConfigurationApplyRetainsTerminalMarkersUntilFreshSuccess) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto old_intent = Intent(CellAdmissionClaimKindDTO::kManagerApply,
                           owner.GetAuthoringAdmission().admission_generation, "old.failed");
  old_intent.owner_epoch = EmptyLivenessSet().manager_owner_epoch;
  const auto old = owner.AcquireCellAdmissionClaim(old_intent);
  ASSERT_TRUE(old.ok());
  auto fresh = old_intent;
  fresh.request_id = RequestId("fresh.apply");
  fresh.owner_transaction_id = "fresh.transaction";
  fresh.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  const auto before = journal.sequence.load();
  EXPECT_FALSE(owner.AcquireConfigurationApplyClaim(fresh, {}).ok());
  EXPECT_FALSE(
      owner
          .AcquireConfigurationApplyClaim(fresh,
                                          [](const auto&, auto, const auto&, const auto&) {
                                            return Result<void>::Err(ErrorDTO{
                                                "apply_live", "old Apply has no terminal", {}});
                                          })
          .ok());
  EXPECT_EQ(journal.sequence.load(), before);
  int checked = 0;
  const auto acquired = owner.AcquireConfigurationApplyClaim(
      fresh, [&](const auto& intent, auto held, const auto&, const auto& epoch) {
        ++checked;
        EXPECT_EQ(intent, fresh);
        EXPECT_EQ(held.size(), 1U);
        if (!held.empty()) {
          EXPECT_EQ(held.front(), old.value());
        }
        EXPECT_EQ(epoch, EmptyLivenessSet().manager_runtime_epoch);
        return Result<void>::Ok();
      });
  ASSERT_TRUE(acquired.ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  const auto replay =
      owner.AcquireConfigurationApplyClaim(fresh, [&](const auto&, auto, const auto&, const auto&) {
        ++checked;
        return Result<void>::Ok();
      });
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), acquired.value());
  EXPECT_EQ(checked, 1);
  EXPECT_FALSE(owner
                   .ReleaseTerminalApplyClaimsAfterSuccess(
                       acquired.value(), Release(acquired.value()),
                       [](const auto&, const auto&, auto) {
                         return Result<void>::Err(
                             ErrorDTO{"fence_held", "old fence remains held", {}});
                       })
                   .ok());
  const auto settled = owner.ReleaseTerminalApplyClaimsAfterSuccess(
      acquired.value(), Release(acquired.value()),
      [&](const auto& claim, const auto&, auto markers) {
        EXPECT_EQ(claim, acquired.value());
        EXPECT_EQ(markers.size(), 1U);
        EXPECT_EQ(markers.front(), old.value());
        return Result<void>::Ok();
      });
  ASSERT_TRUE(settled.ok());
  EXPECT_FALSE(settled.value().authoring_allowed);  // Fresh exclusion remains held.
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(acquired.value())).ok());
  EXPECT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);
}

TEST(KernelCellAdmissionOwner, ConfigurationApplyCannotBypassForeignClaimsOrTrueStopFault) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const auto foreign =
      owner.AcquireCellAdmissionClaim(Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                                             owner.GetAuthoringAdmission().admission_generation));
  ASSERT_TRUE(foreign.ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kManagerApply,
                       owner.GetAuthoringAdmission().admission_generation, "fresh.apply");
  intent.owner_epoch = EmptyLivenessSet().manager_owner_epoch;
  int checked = 0;
  const auto validate = [&](const auto&, auto, const auto&, const auto&) {
    ++checked;
    return Result<void>::Ok();
  };
  EXPECT_FALSE(owner.AcquireConfigurationApplyClaim(intent, validate).ok());
  EXPECT_EQ(checked, 0);
  bool fault = true;
  auto faulted = Owner(clock, journal, 16U, {}, [&] { return fault; });
  ASSERT_TRUE(faulted.UpdateCellAdmissionReadiness(Ready()).ok());
  intent.expected_admission_generation = faulted.GetAuthoringAdmission().admission_generation;
  EXPECT_FALSE(faulted.AcquireConfigurationApplyClaim(intent, validate).ok());
  EXPECT_EQ(checked, 0);
}

TEST(KernelCellAdmissionOwner, ConfigurationApplyProvenWriteFailureDoesNotChangeAdmission) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const auto before = owner.GetAuthoringAdmission();
  auto intent = Intent(CellAdmissionClaimKindDTO::kManagerApply, before.admission_generation);
  intent.owner_epoch = EmptyLivenessSet().manager_owner_epoch;
  journal.fail_configuration_write = true;
  const auto result = owner.AcquireConfigurationApplyClaim(
      intent, [](const auto&, auto, const auto&, const auto&) { return Result<void>::Ok(); });
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(owner.GetAuthoringAdmission(), before);
  EXPECT_FALSE(owner.has_unresolved_claims());
}

TEST(KernelCellAdmissionOwner, ApplyBringUpRequiresPositivePrivatePlanAndKeepsFaultClosed) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal, 16U, {}, [] { return true; });
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kManagerApply,
                       owner.GetAuthoringAdmission().admission_generation, "apply.bring-up");
  intent.owner_epoch = EmptyLivenessSet().manager_owner_epoch;
  intent.owner_transaction_id = "apply.bring-up.1";
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(intent).ok());
  EXPECT_FALSE(owner.AcquireApplyBringUpClaim(intent, {}).ok());
  const auto before = journal.sequence.load();
  EXPECT_FALSE(owner.AcquireApplyBringUpClaim(
                        intent, [](const auto&, const auto&, const auto&) {
                          return Result<void>::Err(ErrorCode::kInvalidRequest);
                        })
                   .ok());
  EXPECT_EQ(journal.sequence.load(), before);

  int validations = 0;
  const CellAdmissionApplyBringUpValidator validate =
      [&](const auto& checked, const auto& readiness, const auto& manager_epoch) {
        ++validations;
        EXPECT_EQ(checked, intent);
        EXPECT_EQ(readiness, Ready());
        EXPECT_EQ(manager_epoch, EmptyLivenessSet().manager_runtime_epoch);
        return Result<void>::Ok();
      };
  const auto acquired = owner.AcquireApplyBringUpClaim(intent, validate);
  ASSERT_TRUE(acquired.ok());
  EXPECT_EQ(validations, 1);
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  const auto replay = owner.AcquireApplyBringUpClaim(intent, validate);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), acquired.value());
  EXPECT_EQ(validations, 1);  // Replay observes the original claim, never a new effect.

  auto competing = intent;
  competing.request_id = RequestId("apply.bring-up.2");
  competing.owner_transaction_id = "apply.bring-up.2";
  competing.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  EXPECT_FALSE(owner.AcquireApplyBringUpClaim(competing, validate).ok());
  EXPECT_EQ(validations, 1);
  const auto released = owner.ReleaseCellAdmissionClaim(Release(acquired.value()));
  ASSERT_TRUE(released.ok());
  EXPECT_FALSE(released.value().authoring_allowed);
  competing.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(competing).ok());
}

TEST(KernelCellAdmissionOwner, ApplyBringUpKeepsOtherExclusionsAndRefusesForeignClaimShape) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const CellAdmissionApplyBringUpValidator validate =
      [](const auto&, const auto&, const auto&) { return Result<void>::Ok(); };
  auto intent = Intent(CellAdmissionClaimKindDTO::kManagerApply,
                       owner.GetAuthoringAdmission().admission_generation);
  intent.owner_epoch = EmptyLivenessSet().manager_owner_epoch;
  auto foreign = intent;
  foreign.claim_kind = CellAdmissionClaimKindDTO::kAuthoringMutation;
  foreign.owner_role = CellAdmissionOwnerRoleDTO::kAuthoringOwner;
  EXPECT_FALSE(owner.AcquireApplyBringUpClaim(foreign, validate).ok());
  auto scoped = intent;
  scoped.scope.resources = {ResourceId("resource.1")};
  EXPECT_FALSE(owner.AcquireApplyBringUpClaim(scoped, validate).ok());
  ASSERT_TRUE(owner.SetRouteRecoveryPending(true).ok());
  intent.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  EXPECT_FALSE(owner.AcquireApplyBringUpClaim(intent, validate).ok());
  ASSERT_TRUE(owner.SetRouteRecoveryPending(false).ok());
  intent.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  const auto ordinary = owner.AcquireCellAdmissionClaim(intent);
  ASSERT_TRUE(ordinary.ok());
  auto different = intent;
  different.request_id = RequestId("different.apply");
  different.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  EXPECT_FALSE(owner.AcquireApplyBringUpClaim(different, validate).ok());
}

TEST(KernelCellAdmissionOwner, CleanApplyBringUpReleaseNeedsNoAdditionalReset) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).value().authoring_allowed);
  auto intent = Intent(CellAdmissionClaimKindDTO::kManagerApply,
                             owner.GetAuthoringAdmission().admission_generation);
  intent.owner_epoch = EmptyLivenessSet().manager_owner_epoch;
  const auto acquired = owner.AcquireApplyBringUpClaim(
      intent, [](const auto&, const auto&, const auto&) { return Result<void>::Ok(); });
  ASSERT_TRUE(acquired.ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  const auto released = owner.ReleaseCellAdmissionClaim(Release(acquired.value()));
  ASSERT_TRUE(released.ok());
  EXPECT_TRUE(released.value().authoring_allowed);
}

TEST(KernelCellAdmissionOwner, ApplyBringUpCannotWaiveCurrentProgramAndOwnerDependencies) {
  for (unsigned scenario = 0U; scenario != 7U; ++scenario) {
    SCOPED_TRACE(scenario);
    FixedClock clock;
    AdmissionJournal journal;
    auto owner = Owner(clock, journal);
    auto readiness = Ready();
    if (scenario == 0U) readiness.required_component_status_current = false;
    if (scenario == 1U) readiness.deployment_status_current = false;
    if (scenario == 2U) readiness.program_cycle_status_current = false;
    if (scenario == 3U) readiness.unresolved_owner_count = 1U;
    if (scenario == 4U) readiness.unsettled_effect_count = 1U;
    if (scenario == 5U) {
      readiness.program_cycle_status->state = ProgramCycleStateDTO::kStarting;
      readiness.program_cycle_status->program_run_id = ProgramRunId("run.pending");
    }
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(readiness).ok());
    auto intent = Intent(CellAdmissionClaimKindDTO::kManagerApply,
                               owner.GetAuthoringAdmission().admission_generation);
    intent.owner_epoch = scenario == 6U ? RequestOwnerEpoch("foreign.manager")
                                      : EmptyLivenessSet().manager_owner_epoch;
    unsigned validations = 0U;
    const auto result = owner.AcquireApplyBringUpClaim(intent, [&](const auto&, const auto&, const auto&) {
      ++validations;
      return Result<void>::Ok();
    });
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(validations, 0U);
  }
}

TEST(KernelCellAdmissionOwner, RouteRecoveryBarrierSurvivesReadyUpdatesAndClaimSettlement) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).value().authoring_allowed);
  const auto lifecycle = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kComponentLifecycle,
             owner.GetAuthoringAdmission().admission_generation, "lifecycle"));
  ASSERT_TRUE(lifecycle.ok());
  EXPECT_TRUE(owner.has_unresolved_claims());
  EXPECT_FALSE(
      owner.has_unresolved_claims_except(CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner));
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(lifecycle.value())).ok());
  auto authoring = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
             owner.GetAuthoringAdmission().admission_generation, "apply"));
  ASSERT_TRUE(authoring.ok());
  EXPECT_TRUE(
      owner.has_unresolved_claims_except(CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner));
  ASSERT_TRUE(owner.SetRouteRecoveryPending(true).ok());
  EXPECT_FALSE(
      owner.ReleaseCellAdmissionClaim(Release(authoring.value())).value().authoring_allowed);
  EXPECT_FALSE(owner.UpdateCellAdmissionReadiness(Ready()).value().authoring_allowed);
  for (auto kind :
       {CellAdmissionClaimKindDTO::kProgramCycle, CellAdmissionClaimKindDTO::kAuthoringMutation,
        CellAdmissionClaimKindDTO::kComponentLifecycle,
        CellAdmissionClaimKindDTO::kManualOperation}) {
    const auto rejected = owner.AcquireCellAdmissionClaim(
        Intent(kind, owner.GetAuthoringAdmission().admission_generation));
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().details.at("reason"), "route_recovery_pending");
  }
  auto recovery = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kInterventionOrRecovery,
             owner.GetAuthoringAdmission().admission_generation, "restore"));
  ASSERT_TRUE(recovery.ok());
  EXPECT_FALSE(
      owner.ReleaseCellAdmissionClaim(Release(recovery.value())).value().authoring_allowed);
  ASSERT_TRUE(owner.SetRouteRecoveryPending(false).ok());
  EXPECT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);

  journal.fail_generation = true;
  EXPECT_FALSE(owner.SetRouteRecoveryPending(true).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  EXPECT_FALSE(owner.SetRouteRecoveryPending(false).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  journal.fail_generation = false;
  ASSERT_TRUE(owner.SetRouteRecoveryPending(false).ok());
  EXPECT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);
}

TEST(KernelCellAdmissionOwner,
     DisabledRequiredComponentCanBeEnabledOnlyFromWholeCellIdle) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  auto readiness = Ready();
  readiness.required_component_current_compatible_ready_converged = 1U;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(readiness).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  const auto generation = owner.GetAuthoringAdmission().admission_generation;
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, generation)).ok());
  const auto lifecycle = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kComponentLifecycle, generation));
  ASSERT_TRUE(lifecycle.ok());
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kProgramCycle,
             owner.GetAuthoringAdmission().admission_generation)).ok());
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(lifecycle.value())).ok());
  readiness.program_cycle_status->state = ProgramCycleStateDTO::kRunning;
  readiness.program_cycle_status->cycle_generation = 2U;
  readiness.program_cycle_status->program_run_id = ProgramRunId("run.1");
  readiness.program_cycle_status->cycle_claim_generation =
      CellAdmissionClaimGeneration(1U);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(readiness).ok());
  const auto busy = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kComponentLifecycle,
             owner.GetAuthoringAdmission().admission_generation, "busy"));
  ASSERT_FALSE(busy.ok());
  // The refusal names what must settle, not only that the cell is busy.
  EXPECT_EQ(busy.error().details.at("reason"),
            "component_lifecycle_program_not_idle_after_reset");
}

TEST(KernelCellAdmissionOwner, ProgramChildrenReplayAfterHoldAndDrainBeforeReset) {
  FixedClock clock;
  AdmissionJournal journal;
  bool held = false;
  unsigned checks = 0;
  auto ready = Ready();
  auto owner = Owner(clock, journal, 8U, [&](const auto&) {
    ++checks;
    return held ? Result<void>::Err(ErrorDTO{"held", "selected node is held", {}})
                : Result<void>::Ok();
  }, {}, [&](const auto, const auto&, const auto&) {
    return Result<ProgramCycleStatusDTO>::Ok(*ready.program_cycle_status);
  });
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  auto parent_intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                              owner.GetAuthoringAdmission().admission_generation);
  parent_intent.owner_epoch = RequestOwnerEpoch("program.owner");
  parent_intent.owner_transaction_id = "run.1";
  const auto parent = owner.AcquireCellAdmissionClaim(parent_intent);
  ASSERT_TRUE(parent.ok());
  ready.program_cycle_status =
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 2U,
                            ProgramCycleStateDTO::kRunning,     ProgramRunId("run.1"),
                            parent.value().claim_generation,    std::nullopt};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  auto child = Intent(CellAdmissionClaimKindDTO::kAuthoredScene,
                      owner.GetAuthoringAdmission().admission_generation, "scene.child");
  child.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{RequestOwnerEpoch("program.owner"),
                                                                ProgramRunId("run.1"),
                                                                parent.value().claim_generation,
                                                                "attach",
                                                                "attach.attempt.1",
                                                                "scene.carried_attachment"};
  const auto scene = owner.AcquireCellAdmissionClaim(child);
  ASSERT_TRUE(scene.ok()) << scene.error().message;
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  EXPECT_EQ(owner.unsettled_program_child_nodes(parent.value().claim_generation),
            std::vector<std::string>{"attach"});
  auto lifecycle = Intent(CellAdmissionClaimKindDTO::kComponentLifecycle,
                          owner.GetAuthoringAdmission().admission_generation, "lifecycle.child");
  lifecycle.program_node_evidence =
      ProgramNodeAdmissionEvidenceDTO{RequestOwnerEpoch("program.owner"),
                                      ProgramRunId("run.1"),
                                      parent.value().claim_generation,
                                      "enable",
                                      "enable.attempt.1",
                                      "cclc.lifecycle"};
  const auto enabled = owner.AcquireCellAdmissionClaim(lifecycle);
  ASSERT_FALSE(enabled.ok());
  // Lifecycle is not a Program node: its evidence is rejected as invalid.
  EXPECT_EQ(enabled.error().details.at("reason"), "program_node_evidence_invalid");
  held = true;
  const auto before = journal.sequence.load();
  ASSERT_EQ(owner.AcquireCellAdmissionClaim(child).value(), scene.value());
  EXPECT_EQ(checks, 1U);  // Refusal and exact replay never re-enter effect admission.
  EXPECT_EQ(journal.sequence.load(), before);
  auto late = child;
  late.request_id = RequestId("scene.late");
  // A Stop fence is the relevant no-admission outcome even if an unrelated
  // readiness publication or concurrent child has advanced the generation.
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(late).error().code, "held");
  late.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(late).error().code, "held");
  EXPECT_EQ(journal.sequence.load(), before);
  ready.program_cycle_status->state = ProgramCycleStateDTO::kResetting;
  ready.program_cycle_status->accepted_reset = AcceptedReset();
  ready.program_cycle_status->cycle_generation = 3U;
  ready.program_cycle_status->terminal_or_settlement_reference = ContentDigest(kDigest);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  late.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(late).error().details.at("reason"),
            "program_cycle_resetting");
  const ProgramResetSceneAuthorizationDTO reset{
      RequestOwnerEpoch("program.owner"), ProgramRunId("run.1"), parent.value().claim_generation,
      ContentDigest(kDigest)};
  EXPECT_FALSE(owner.ValidateProgramResetSceneAuthorization(reset).ok());
  EXPECT_FALSE(owner.ReleaseCellAdmissionClaim(Release(parent.value())).ok());
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(scene.value())).ok());
  EXPECT_TRUE(owner.unsettled_program_child_nodes(parent.value().claim_generation).empty());
  EXPECT_TRUE(owner.ValidateProgramResetSceneAuthorization(reset).ok());
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
                                       const ManagerLivenessRequiredSetDTO& set,
                                       CellAdmissionProgramResetStatusSource reset_status_source = {}) {
  return KernelCellAdmissionOwner(
      RequestOwnerEpoch("kernel.1"), 16U, clock, journal, {}, AdmissionGeneration(0U),
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&, const auto& reconcile) {
        return Result<CellAdmissionReconcileValidationDTO>::Ok(
            {reconcile.current_owner_epochs, {}});
      },
      {}, {}, set, CellAdmissionClaimGeneration(0U), {}, {}, std::move(reset_status_source));
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
  acquiring.program_cycle_status->state = ProgramCycleStateDTO::kRecoveryRequired;
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
     ExactStartingProgramCycleMayAcquireWhileOrdinaryAuthoringRemainsClosed) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  auto starting = Ready();
  starting.program_cycle_status = ProgramCycleStatusDTO{
      RequestOwnerEpoch("owner.1"),  1U,           ProgramCycleStateDTO::kStarting,
      ProgramRunId("transaction.1"), std::nullopt, std::nullopt};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(starting).ok());
  const auto admission = owner.GetAuthoringAdmission();
  ASSERT_FALSE(admission.authoring_allowed);

  EXPECT_EQ(owner
                .AcquireCellAdmissionClaim(Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                                                  admission.admission_generation,
                                                  "authoring.while-starting"))
                .error()
                .code,
            "authoring_not_ready");
  const auto cycle =
      owner.AcquireCellAdmissionClaim(Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                                             admission.admission_generation, "program.starting"));
  ASSERT_TRUE(cycle.ok());
  EXPECT_EQ(cycle.value().intent.owner_epoch, RequestOwnerEpoch("owner.1"));
  EXPECT_EQ(cycle.value().intent.owner_transaction_id, "transaction.1");
}

TEST(KernelCellAdmissionOwner,
     StartingProgramCycleExceptionIsExactAndFailsClosedForEveryOtherCase) {
  const auto rejected = [](auto mutate_readiness, auto mutate_intent) {
    FixedClock clock;
    AdmissionJournal journal;
    auto owner = Owner(clock, journal);
    auto starting = Ready();
    starting.program_cycle_status = ProgramCycleStatusDTO{
        RequestOwnerEpoch("owner.1"),  1U,           ProgramCycleStateDTO::kStarting,
        ProgramRunId("transaction.1"), std::nullopt, std::nullopt};
    mutate_readiness(starting);
    EXPECT_TRUE(owner.UpdateCellAdmissionReadiness(starting).ok());
    auto intent =
        Intent(CellAdmissionClaimKindDTO::kProgramCycle,
               owner.GetAuthoringAdmission().admission_generation, "program.starting.rejected");
    mutate_intent(intent);
    const auto result = owner.AcquireCellAdmissionClaim(intent);
    EXPECT_FALSE(result.ok());
    if (!result.ok()) {
      EXPECT_EQ(result.error().code, "authoring_not_ready");
    }
  };
  const auto unchanged_readiness = [](auto&) {};
  const auto unchanged_intent = [](auto&) {};

  rejected(unchanged_readiness,
           [](auto& intent) { intent.owner_epoch = RequestOwnerEpoch("foreign.program.owner"); });
  rejected(unchanged_readiness,
           [](auto& intent) { intent.owner_transaction_id = "program-cycle.foreign"; });
  rejected(
      [](auto& readiness) {
        readiness.program_cycle_status->state = ProgramCycleStateDTO::kStopping;
      },
      unchanged_intent);
  rejected([](auto& readiness) { readiness.blocking_fault_count = 1U; }, unchanged_intent);
  rejected(
      [](auto& readiness) {
        readiness.program_cycle_status->cycle_claim_generation = CellAdmissionClaimGeneration(7U);
      },
      unchanged_intent);
  rejected(
      [](auto& readiness) {
        readiness.program_cycle_status->terminal_or_settlement_reference = ContentDigest(kDigest);
      },
      unchanged_intent);
  rejected([](auto& readiness) { readiness.program_cycle_status_current = false; },
           unchanged_intent);
  rejected([](auto& readiness) { readiness.required_component_status_current = false; },
           unchanged_intent);
  rejected([](auto& readiness) { readiness.deployment_status_current = false; }, unchanged_intent);
  rejected([](auto& readiness) { readiness.deployment_converged = false; }, unchanged_intent);
  rejected([](auto& readiness) { readiness.unsettled_effect_count = 1U; }, unchanged_intent);
  rejected([](auto& readiness) { readiness.unresolved_owner_count = 1U; }, unchanged_intent);
}

TEST(KernelCellAdmissionOwner, StartingClaimNeverBypassesExpiredManagerAndHasOneConcurrentWinner) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto set = LivenessSet();
  auto owner = LivenessOwner(clock, journal, set);
  auto starting = Ready();
  starting.program_cycle_status = ProgramCycleStatusDTO{
      RequestOwnerEpoch("owner.1"),  1U,           ProgramCycleStateDTO::kStarting,
      ProgramRunId("transaction.1"), std::nullopt, std::nullopt};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(starting).ok());
  for (std::size_t receiver = 0; receiver < set.required_receivers.size(); ++receiver) {
    ASSERT_TRUE(
        owner.Publish(LivenessObservation(set, receiver, 1U, ManagerLivenessStateDTO::kCurrent))
            .ok());
  }
  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 2U, ManagerLivenessStateDTO::kExpired, true))
          .ok());
  EXPECT_EQ(owner
                .AcquireCellAdmissionClaim(
                    Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                           owner.GetAuthoringAdmission().admission_generation, "starting.expired"))
                .error()
                .code,
            "authoring_not_ready");

  // A separate healthy owner isolates the actual admission race, with no
  // sleeps and no attempt to reopen an expired Manager incarnation.
  auto healthy = Owner(clock, journal);
  ASSERT_TRUE(healthy.UpdateCellAdmissionReadiness(starting).ok());
  const auto generation = healthy.GetAuthoringAdmission().admission_generation;
  std::barrier gate(3);
  std::atomic<unsigned> winners{};
  const auto compete = [&](const char* id) {
    gate.arrive_and_wait();
    if (healthy
            .AcquireCellAdmissionClaim(
                Intent(CellAdmissionClaimKindDTO::kProgramCycle, generation, id))
            .ok())
      ++winners;
  };
  std::thread first(compete, "starting.first");
  std::thread second(compete, "starting.second");
  gate.arrive_and_wait();
  first.join();
  second.join();
  EXPECT_EQ(winners.load(), 1U);
  EXPECT_FALSE(healthy.GetAuthoringAdmission().authoring_allowed);
}

TEST(KernelCellAdmissionOwner, ProgramResetSceneAuthorizationRequiresExactResettingCycleAndClaim) {
  FixedClock clock;
  AdmissionJournal journal;
  auto resetting = Ready();
  auto owner = Owner(clock, journal, 16U, {}, {}, [&](const auto, const auto&, const auto&) {
    return Result<ProgramCycleStatusDTO>::Ok(*resetting.program_cycle_status);
  });
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                       owner.GetAuthoringAdmission().admission_generation, "program-reset.claim");
  intent.owner_epoch = RequestOwnerEpoch("program.owner");
  intent.owner_transaction_id = "program-cycle.2";
  const auto acquired = owner.AcquireCellAdmissionClaim(intent);
  ASSERT_TRUE(acquired.ok());
  const ProgramResetSceneAuthorizationDTO authorization{
      RequestOwnerEpoch("program.owner"), ProgramRunId("program-cycle.2"),
      acquired.value().claim_generation, ContentDigest(kDigest)};

  EXPECT_EQ(
      owner.ValidateProgramResetSceneAuthorization(authorization).error().details.at("reason"),
      "program_reset_status_mismatch");

  auto child_intent =
      Intent(CellAdmissionClaimKindDTO::kInterventionOrRecovery,
             owner.GetAuthoringAdmission().admission_generation, "program-reset.child");
  child_intent.intervention_evidence->parent_program_cycle_claim_generation =
      acquired.value().claim_generation;
  const auto child = owner.AcquireCellAdmissionClaim(child_intent);
  ASSERT_TRUE(child.ok());

  resetting.program_cycle_status =
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 2U,
                            ProgramCycleStateDTO::kResetting,   ProgramRunId("program-cycle.2"),
                            acquired.value().claim_generation,  ContentDigest(kDigest), AcceptedReset()};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(resetting).ok());
  const auto blocked_reset = owner.ValidateProgramResetSceneAuthorization(authorization);
  ASSERT_FALSE(blocked_reset.ok());
  EXPECT_EQ(blocked_reset.error().details.at("reason"), "program_reset_child_unsettled");

  auto stale = authorization;
  stale.terminal_or_settlement_reference = ContentDigest(kDigestB);
  EXPECT_EQ(owner.ValidateProgramResetSceneAuthorization(stale).error().details.at("reason"),
            "program_reset_status_mismatch");

  const auto sequence_before_replay = journal.sequence.load();
  const auto replay = owner.AcquireCellAdmissionClaim(child_intent);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), child.value());
  EXPECT_EQ(journal.sequence.load(), sequence_before_replay);
  EXPECT_FALSE(owner.ReleaseCellAdmissionClaim(Release(acquired.value())).ok());
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(child.value())).ok());
  EXPECT_TRUE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());

  // Simulate the gap after authorization and before Scene commit. A new child
  // uses the current admission generation, so rejection is the Reset fence,
  // not an incidental stale-generation or overlapping-child rejection.
  child_intent.request_id = RequestId("program-reset.late-child");
  child_intent.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  const auto sequence_before_rejection = journal.sequence.load();
  const auto late_child = owner.AcquireCellAdmissionClaim(child_intent);
  ASSERT_FALSE(late_child.ok());
  EXPECT_EQ(late_child.error().details.at("reason"), "program_cycle_resetting");
  EXPECT_EQ(journal.sequence.load(), sequence_before_rejection);
  EXPECT_TRUE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
}

TEST(KernelCellAdmissionOwner, SelectedIdleResetRequiresOriginalAcceptanceAndExclusiveClaim) {
  FixedClock clock;
  AdmissionJournal journal;
  auto ready = Ready();
  auto owner = Owner(clock, journal, 16U, {}, {}, [&](const auto, const auto&, const auto&) {
    return Result<ProgramCycleStatusDTO>::Ok(*ready.program_cycle_status);
  });
  ready.program_cycle_status->state = ProgramCycleStateDTO::kResetting;
  ready.program_cycle_status->accepted_reset = AcceptedReset();
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  const auto identity = ProgramResetClaimRequestId(*ready.program_cycle_status);
  ASSERT_TRUE(identity.ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
                       owner.GetAuthoringAdmission().admission_generation,
                       identity.value().value());
  intent.owner_epoch = ready.program_cycle_status->program_runner_owner_epoch;
  intent.owner_transaction_id = identity.value().value();
  auto foreign = intent;
  foreign.request_payload_digest = ContentDigest(kDigestB);
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(foreign).ok());
  const auto acquired = owner.AcquireCellAdmissionClaim(intent);
  ASSERT_TRUE(acquired.ok());
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(intent).value(), acquired.value());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  ready.program_cycle_status->accepted_reset->reset_claim_generation = acquired.value().claim_generation;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  const ProgramResetSceneAuthorizationDTO authorization{
      intent.owner_epoch, ProgramRunId(""), CellAdmissionClaimGeneration(0U), ContentDigest(""),
      ProgramResetClaimSceneAuthorizationDTO{
          ready.program_cycle_status->cycle_generation,
          AcceptedReset().request_owner_epoch, AcceptedReset().request_id,
          AcceptedReset().request_payload_digest, acquired.value().claim_generation}};
  EXPECT_TRUE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
  auto stale = authorization;
  stale.reset_claim->request_id = RequestId("foreign.reset");
  EXPECT_FALSE(owner.ValidateProgramResetSceneAuthorization(stale).ok());
  stale = authorization;
  stale.program_run_id = ProgramRunId("invented.run");
  EXPECT_FALSE(ValidateProgramResetSceneAuthorization(stale).ok());
  for (const auto kind : {CellAdmissionClaimKindDTO::kProgramCycle,
                          CellAdmissionClaimKindDTO::kAuthoringMutation,
                          CellAdmissionClaimKindDTO::kComponentLifecycle}) {
    EXPECT_FALSE(owner.AcquireCellAdmissionClaim(
        Intent(kind, owner.GetAuthoringAdmission().admission_generation, "concurrent")).ok());
  }
  ready.program_cycle_status->accepted_reset->reload_initial_scene = false;
  ready.program_cycle_status->accepted_reset->reset_robot_position = true;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  EXPECT_FALSE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(acquired.value())).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  EXPECT_FALSE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
}

TEST(KernelCellAdmissionOwner, NoRunResetProjectionRequiresSelectedAcceptedIdentity) {
  auto status = *Ready().program_cycle_status;
  status.state = ProgramCycleStateDTO::kResetting;
  EXPECT_FALSE(ValidateProgramCycleStatus(status).ok());
  status.accepted_reset = AcceptedReset();
  EXPECT_TRUE(ValidateProgramCycleStatus(status).ok());
  const auto identity = ProgramResetClaimRequestId(status);
  ASSERT_TRUE(identity.ok());
  status.accepted_reset->reset_claim_generation = CellAdmissionClaimGeneration(5U);
  EXPECT_EQ(ProgramResetClaimRequestId(status).value(), identity.value());
  status.accepted_reset->reset_claim_generation = CellAdmissionClaimGeneration(0U);
  EXPECT_FALSE(ValidateProgramCycleStatus(status).ok());
  status.accepted_reset = AcceptedReset();
  status.accepted_reset->reload_initial_scene = false;
  EXPECT_FALSE(ValidateProgramCycleStatus(status).ok());
  status.accepted_reset = AcceptedReset();
  status.state = ProgramCycleStateDTO::kIdleAfterReset;
  EXPECT_FALSE(ValidateProgramCycleStatus(status).ok());
  status.state = ProgramCycleStateDTO::kRecoveryRequired;
  EXPECT_TRUE(ValidateProgramCycleStatus(status).ok());
}

TEST(KernelCellAdmissionOwner, SelectedResetGrantsRefuseMissingAuthoritativePhaseSource) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  auto ready = Ready();
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  auto parent_intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
      owner.GetAuthoringAdmission().admission_generation);
  parent_intent.owner_epoch = ready.program_cycle_status->program_runner_owner_epoch;
  parent_intent.owner_transaction_id = "run.phase-proof";
  const auto parent = owner.AcquireCellAdmissionClaim(parent_intent);
  ASSERT_TRUE(parent.ok());
  ready.program_cycle_status = ProgramCycleStatusDTO{parent_intent.owner_epoch, 2U,
      ProgramCycleStateDTO::kResetting, ProgramRunId(parent_intent.owner_transaction_id),
      parent.value().claim_generation, ContentDigest(kDigest), AcceptedReset()};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  const auto refused_scene = owner.ValidateProgramResetSceneAuthorization({parent_intent.owner_epoch,
      ProgramRunId(parent_intent.owner_transaction_id), parent.value().claim_generation, ContentDigest(kDigest)});
  ASSERT_FALSE(refused_scene.ok());
  EXPECT_EQ(refused_scene.error().details.at("reason"), "program_reset_phase_proof_unavailable");
  auto position = owner.PrepareProgramResetPositionAuthorization({parent_intent.owner_epoch, 2U,
      AcceptedReset().request_owner_epoch, AcceptedReset().request_id, AcceptedReset().request_payload_digest,
      ProgramRunId(parent_intent.owner_transaction_id), parent.value().claim_generation,
      ContentDigest(kDigest), std::nullopt});
  ASSERT_FALSE(position.ok());
  EXPECT_EQ(position.error().details.at("reason"), "program_reset_phase_proof_unavailable");

  AdmissionJournal idle_journal;
  auto idle = Owner(clock, idle_journal);
  ready = Ready();
  ready.program_cycle_status->state = ProgramCycleStateDTO::kResetting;
  ready.program_cycle_status->accepted_reset = AcceptedReset();
  ASSERT_TRUE(idle.UpdateCellAdmissionReadiness(ready).ok());
  const auto identity = ProgramResetClaimRequestId(*ready.program_cycle_status);
  ASSERT_TRUE(identity.ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
      idle.GetAuthoringAdmission().admission_generation, identity.value().value());
  intent.owner_epoch = ready.program_cycle_status->program_runner_owner_epoch;
  intent.owner_transaction_id = identity.value().value();
  const auto before = idle_journal.sequence.load();
  const auto refused_acquire = idle.AcquireCellAdmissionClaim(intent);
  ASSERT_FALSE(refused_acquire.ok());
  EXPECT_EQ(refused_acquire.error().details.at("reason"), "program_reset_phase_proof_unavailable");
  EXPECT_EQ(idle_journal.sequence.load(), before);
  EXPECT_FALSE(idle.has_unresolved_claims());
}

TEST(KernelCellAdmissionOwner, ExactResetJournalJoinsCurrentOwnerWithoutWaitingForHeartbeat) {
  FixedClock clock;
  AdmissionJournal journal;
  auto actual = *Ready().program_cycle_status;
  actual.state = ProgramCycleStateDTO::kResetting;
  actual.accepted_reset = AcceptedReset();
  unsigned reads = 0U;
  bool unavailable = false;
  auto owner = Owner(clock, journal, 16U, {}, {},
      [&](const auto purpose, const auto&, const auto& claim) {
        ++reads;
        EXPECT_EQ(purpose == ProgramResetJournalReadPurpose::kAcquire, !claim.has_value());
        if (unavailable) return Result<ProgramCycleStatusDTO>::Err(ErrorCode::kMissingReference);
        return Result<ProgramCycleStatusDTO>::Ok(actual);
      });
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const auto identity = ProgramResetClaimRequestId(actual);
  ASSERT_TRUE(identity.ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
                       owner.GetAuthoringAdmission().admission_generation, identity.value().value());
  intent.owner_epoch = actual.program_runner_owner_epoch;
  intent.owner_transaction_id = identity.value().value();
  unavailable = true;
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(intent).ok());
  unavailable = false;
  const auto acquired = owner.AcquireCellAdmissionClaim(intent);
  ASSERT_TRUE(acquired.ok());
  const auto read_count = reads;
  ASSERT_TRUE(owner.AcquireCellAdmissionClaim(intent).ok());
  EXPECT_EQ(reads, read_count);
  actual.accepted_reset->reset_claim_generation = acquired.value().claim_generation;
  const ProgramResetSceneAuthorizationDTO authorization{
      actual.program_runner_owner_epoch, ProgramRunId(""), CellAdmissionClaimGeneration(0U), ContentDigest(""),
      ProgramResetClaimSceneAuthorizationDTO{actual.cycle_generation,
          actual.accepted_reset->request_owner_epoch, actual.accepted_reset->request_id,
          actual.accepted_reset->request_payload_digest, acquired.value().claim_generation}};
  EXPECT_TRUE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
  actual.program_runner_owner_epoch = RequestOwnerEpoch("foreign.runner");
  EXPECT_FALSE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
  actual.program_runner_owner_epoch = intent.owner_epoch;
  actual.cycle_generation += 1U;
  EXPECT_FALSE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
}

TEST(KernelCellAdmissionOwner, BlockedResetJournalReadDoesNotBlockManagerLossAndRejectsStaleProof) {
  for (const bool run_scene : {false, true}) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto set = LivenessSet({{RuntimeUnitId("runtime.a"), RuntimeEpoch("runtime.a.epoch"),
                                 RuntimePolicyId("policy.1"), RuntimePolicyRevision("1")}});
  auto actual = *Ready().program_cycle_status;
  actual.state = ProgramCycleStateDTO::kResetting;
  actual.accepted_reset = AcceptedReset();
  std::promise<void> reading;
  std::promise<void> finish_read;
  const auto finish = finish_read.get_future().share();
  auto owner = LivenessOwner(clock, journal, set, [&](const auto, const auto&, const auto&) {
    reading.set_value();
    finish.wait();
    return Result<ProgramCycleStatusDTO>::Ok(actual);
  });
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  ASSERT_TRUE(owner.Publish(LivenessObservation(set, 0U, 1U, ManagerLivenessStateDTO::kCurrent)).ok());
  std::optional<CellAdmissionClaimDTO> parent;
  if (run_scene) {
    auto parent_intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
        owner.GetAuthoringAdmission().admission_generation);
    parent_intent.owner_epoch = actual.program_runner_owner_epoch;
    parent_intent.owner_transaction_id = "run.1";
    const auto acquired = owner.AcquireCellAdmissionClaim(parent_intent);
    ASSERT_TRUE(acquired.ok());
    parent = acquired.value();
    actual.program_run_id = ProgramRunId("run.1");
    actual.cycle_claim_generation = parent->claim_generation;
    actual.terminal_or_settlement_reference = ContentDigest(kDigest);
    auto ready = Ready();
    ready.program_cycle_status = actual;
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  }
  const auto identity = ProgramResetClaimRequestId(actual);
  ASSERT_EQ(identity.ok(), !run_scene);
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
      owner.GetAuthoringAdmission().admission_generation, run_scene ? "unused" : identity.value().value());
  intent.owner_epoch = actual.program_runner_owner_epoch;
  intent.owner_transaction_id = run_scene ? "unused" : identity.value().value();
  auto acquire = std::async(std::launch::async, [&] {
    if (!run_scene) return owner.AcquireCellAdmissionClaim(intent).ok();
    return owner.ValidateProgramResetSceneAuthorization({actual.program_runner_owner_epoch,
        *actual.program_run_id, parent->claim_generation,
        *actual.terminal_or_settlement_reference}).ok();
  });
  reading.get_future().wait();
  const ManagerLivenessLossIntentDTO loss{set.cell_id, set.active_bundle_id,
      set.required_runtime_set_digest, set.required_receivers.front().runtime_unit_id,
      set.required_receivers.front().runtime_epoch, set.required_receivers.front().runtime_policy_id,
      set.required_receivers.front().runtime_policy_revision, set.manager_runtime_unit_id,
      set.manager_runtime_epoch, set.manager_owner_epoch, 2U, ManagerLivenessLossKindDTO::kExpiry};
  auto containment = std::async(std::launch::async, [&] { return owner.LatchManagerLivenessLoss(loss); });
  const auto ready = containment.wait_for(std::chrono::seconds(1));
  finish_read.set_value();
  EXPECT_EQ(ready, std::future_status::ready);
  EXPECT_TRUE(containment.get());
  EXPECT_FALSE(acquire.get());
  EXPECT_EQ(owner.has_unresolved_claims(), run_scene);
}
}

TEST(KernelCellAdmissionOwner, PreparedResetJournalProofCannotSurviveAdmissionGenerationChange) {
  FixedClock clock;
  AdmissionJournal journal;
  auto actual = *Ready().program_cycle_status;
  actual.state = ProgramCycleStateDTO::kResetting;
  actual.accepted_reset = AcceptedReset();
  auto owner = Owner(clock, journal, 16U, {}, {}, [&](const auto, const auto&, const auto&) {
    return Result<ProgramCycleStatusDTO>::Ok(actual);
  });
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const auto identity = ProgramResetClaimRequestId(actual);
  ASSERT_TRUE(identity.ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
      owner.GetAuthoringAdmission().admission_generation, identity.value().value());
  intent.owner_epoch = actual.program_runner_owner_epoch;
  intent.owner_transaction_id = identity.value().value();
  auto proof = owner.PrepareProgramResetStatus(ProgramResetJournalReadPurpose::kAcquire, intent);
  ASSERT_TRUE(proof.ok());
  ASSERT_TRUE(owner.SetRouteRecoveryPending(true).ok());
  ASSERT_TRUE(owner.SetRouteRecoveryPending(false).ok());
  const auto acquired = owner.AcquirePreparedProgramResetClaim(intent, proof.value());
  ASSERT_FALSE(acquired.ok());
  EXPECT_EQ(acquired.error().code, "stale_generation");
}

TEST(KernelCellAdmissionOwner, ResettingReconciledParentAlsoRejectsFreshChildren) {
  FixedClock clock;
  AdmissionJournal journal;
  auto parent_intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle, AdmissionGeneration(1U));
  parent_intent.owner_transaction_id = "program-cycle.1";
  const CellAdmissionClaimDTO parent{parent_intent, CellAdmissionClaimGeneration(1U),
                                     AdmissionGeneration(1U), 41};
  const RequestOwnerEpoch successor("program.successor");
  const CellAdmissionReconcileSettlementDTO settlement{
      parent, CellAdmissionOwnerRoleDTO::kProgramRunner, successor,
      CellAdmissionSettlementKindDTO::kReconciled, ContentDigest(kDigest)};
  auto resetting = Ready();
  resetting.program_cycle_status = ProgramCycleStatusDTO{successor,
                                                         2U,
                                                         ProgramCycleStateDTO::kResetting,
                                                         ProgramRunId("program-cycle.1"),
                                                         parent.claim_generation,
                                                         ContentDigest(kDigest), AcceptedReset()};
  KernelCellAdmissionOwner owner(RequestOwnerEpoch("kernel.2"), 4U, clock, journal,
                                 {{parent, {}, settlement}}, AdmissionGeneration(3U),
                                 [](const auto&, const auto&) { return Result<void>::Ok(); }, {},
                                 {}, {}, {{CellAdmissionOwnerRoleDTO::kProgramRunner, successor}},
                                 EmptyLivenessSet(), CellAdmissionClaimGeneration(0U), {}, {},
                                 [&](const auto, const auto&, const auto&) {
                                   return Result<ProgramCycleStatusDTO>::Ok(*resetting.program_cycle_status);
                                 });
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(resetting).ok());
  const ProgramResetSceneAuthorizationDTO authorization{
      successor, ProgramRunId("program-cycle.1"), parent.claim_generation, ContentDigest(kDigest)};
  ASSERT_TRUE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
  auto child = Intent(CellAdmissionClaimKindDTO::kInterventionOrRecovery,
                      owner.GetAuthoringAdmission().admission_generation, "late-child");
  child.intervention_evidence->parent_program_cycle_claim_generation = parent.claim_generation;
  const auto before = journal.sequence.load();
  const auto admitted = owner.AcquireCellAdmissionClaim(child);
  ASSERT_FALSE(admitted.ok());
  EXPECT_EQ(admitted.error().details.at("reason"), "program_cycle_resetting");
  EXPECT_EQ(journal.sequence.load(), before);
  EXPECT_TRUE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
}

TEST(KernelCellAdmissionOwner, RecoveryRejectsMultipleHeldCyclesAndMissingProgramEvidence) {
  FixedClock clock;
  AdmissionJournal journal;
  const CellAdmissionClaimDTO first{
      Intent(CellAdmissionClaimKindDTO::kProgramCycle, AdmissionGeneration(1U)),
      CellAdmissionClaimGeneration(1U), AdmissionGeneration(1U), 1};
  auto second = first;
  second.claim_generation = CellAdmissionClaimGeneration(2U);
  second.intent.request_id = RequestId("second-claim");
  const std::vector<OperationJournalRecoveryDTO::RecoveredCellAdmissionClaimDTO> duplicate{
      {first, {}, {}}, {second, {}, {}}};
  EXPECT_THROW(
      KernelCellAdmissionOwner(RequestOwnerEpoch("kernel.2"), 8U, clock, journal, duplicate),
      std::runtime_error);
  second.intent.program_cycle_evidence.reset();
  const std::vector<OperationJournalRecoveryDTO::RecoveredCellAdmissionClaimDTO> malformed{
      {second, {}, {}}};
  EXPECT_THROW(
      KernelCellAdmissionOwner(RequestOwnerEpoch("kernel.2"), 8U, clock, journal, malformed),
      std::runtime_error);
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

CellAdmissionClaimIntentDTO RouteRecoveryIntent(AdmissionGeneration generation) {
  auto intent = Intent(CellAdmissionClaimKindDTO::kInterventionOrRecovery, generation);
  intent.owner_role = CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner;
  intent.owner_transaction_id = "route-selection.recovery.1";
  intent.scope.configurations = {{ControlDomainId("arm"), ActiveControlConfigurationId("saved"),
                                  ActiveControlConfigurationGeneration(4U)}};
  intent.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
      intent.owner_role, intent.owner_epoch, intent.owner_transaction_id,
      intent.request_payload_digest, std::nullopt};
  return intent;
}

TEST(KernelCellAdmissionOwner, CCLCRouteRecoveryRequiresExactOwnerOfferAndOneConfiguration) {
  const auto intent = RouteRecoveryIntent(AdmissionGeneration(1U));
  ASSERT_TRUE(ValidateCellAdmissionClaimIntent(intent).ok());
  for (int change = 0; change < 10; ++change) {
    auto bad = intent;
    auto& evidence = *bad.intervention_evidence;
    if (change == 0) evidence.offering_owner_role = CellAdmissionOwnerRoleDTO::kProgramRunner;
    if (change == 1) evidence.offering_owner_epoch = RequestOwnerEpoch("foreign");
    if (change == 2) evidence.offer_transaction_id = "route-selection.foreign";
    if (change == 3) evidence.offer_digest = Digest('b');
    if (change == 4)
      evidence.parent_program_cycle_claim_generation = CellAdmissionClaimGeneration(1U);
    if (change == 5) bad.scope.configurations.clear();
    if (change == 6)
      bad.scope.configurations.push_back({ControlDomainId("other"),
                                          ActiveControlConfigurationId("other"),
                                          ActiveControlConfigurationGeneration(1U)});
    if (change == 7) bad.scope.resources = {ResourceId("arm.joint")};
    if (change == 8) bad.claim_kind = CellAdmissionClaimKindDTO::kComponentLifecycle;
    if (change == 9) {
      bad.owner_transaction_id = "lifecycle.1";
      evidence.offer_transaction_id = bad.owner_transaction_id;
    }
    EXPECT_FALSE(ValidateCellAdmissionClaimIntent(bad).ok()) << change;
  }
}

CellAdmissionClaimIntentDTO SelectedBringUpIntent(AdmissionGeneration generation) {
  auto intent = RouteRecoveryIntent(generation);
  intent.owner_transaction_id = "selected-lifecycle.1";
  intent.scope.resources = {ResourceId("arm.joint"), ResourceId("gripper.position")};
  intent.scope.configurations.push_back({ControlDomainId("gripper"), ActiveControlConfigurationId("tool.saved"),
                                        ActiveControlConfigurationGeneration(2U)});
  intent.intervention_evidence->offer_transaction_id = intent.owner_transaction_id;
  return intent;
}

TEST(KernelCellAdmissionOwner, SelectedBringUpRequiresExactPrivateValidatorAndPhysicalScope) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  auto intent = SelectedBringUpIntent(owner.GetAuthoringAdmission().admission_generation);
  ASSERT_TRUE(ValidateCellAdmissionClaimIntent(intent).ok());
  EXPECT_FALSE(owner.AcquireSelectedBringUpClaim(intent, {}).ok());
  EXPECT_FALSE(
      owner
          .AcquireRouteRecoveryClaim(
              intent,
              [](const auto&, const auto&) {
                ADD_FAILURE()
                    << "observation-only route recovery cannot validate a selected effect offer";
                return Result<void>::Ok();
              })
          .ok());
  for (int change = 0; change < 5; ++change) {
    auto invalid = intent;
    if (change == 0) invalid.scope.resources.clear();
    if (change == 1) invalid.scope.configurations.clear();
    if (change == 2) invalid.intervention_evidence->offer_digest = Digest('b');
    if (change == 3)
      invalid.intervention_evidence->offering_owner_epoch = RequestOwnerEpoch("foreign");
    if (change == 4)
      invalid.intervention_evidence->parent_program_cycle_claim_generation =
          CellAdmissionClaimGeneration(1U);
    EXPECT_FALSE(ValidateCellAdmissionClaimIntent(invalid).ok()) << change;
  }
  unsigned validated = 0;
  const auto rejected = owner.AcquireSelectedBringUpClaim(intent, [&](const auto&, const auto&) {
    ++validated;
    return Result<void>::Err(ErrorDTO{"selected_not_current", "predecessor changed", {}});
  });
  EXPECT_FALSE(rejected.ok());
  EXPECT_EQ(validated, 1U);
  EXPECT_EQ(journal.sequence.load(), 0U);
  const auto acquired =
      owner.AcquireSelectedBringUpClaim(intent, [&](const auto& offered, const auto& parent) {
        ++validated;
        EXPECT_EQ(offered, intent);
        EXPECT_FALSE(parent);
        return Result<void>::Ok();
      });
  ASSERT_TRUE(acquired.ok()) << acquired.error().message;
  EXPECT_EQ(validated, 2U);
  EXPECT_EQ(journal.sequence.load(), 1U);
  const auto replay = owner.AcquireSelectedBringUpClaim(intent, [&](const auto&, const auto&) {
    ADD_FAILURE() << "exact replay must observe existing authority rather than retry an effect";
    return Result<void>::Err(ErrorCode::kInternalError);
  });
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), acquired.value());
  EXPECT_EQ(journal.sequence.load(), 1U);
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
}

TEST(KernelCellAdmissionOwner, CCLCRouteRecoveryDoesNotGrantOrdinaryReadiness) {
  FixedClock clock;
  AdmissionJournal journal;
  bool current_offer = false;
  unsigned checks = 0;
  KernelCellAdmissionOwner owner{
      RequestOwnerEpoch("kernel.1"),
      16U,
      clock,
      journal,
      {},
      AdmissionGeneration(0U),
      [&](const auto& intent, const auto& parent) {
        ++checks;
        return current_offer && !parent &&
                       intent == RouteRecoveryIntent(intent.expected_admission_generation)
                   ? Result<void>::Ok()
                   : Result<void>::Err(ErrorCode::kInvalidStateTransition);
      },
      [](const auto&, const auto&) { return Result<void>::Ok(); }};
  ASSERT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  const auto intent = RouteRecoveryIntent(owner.GetAuthoringAdmission().admission_generation);
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(intent).ok());
  current_offer = true;
  auto foreign = intent;
  foreign.scope.configurations.front().active_configuration_id =
      ActiveControlConfigurationId("foreign");
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(foreign).ok());
  const auto acquired = owner.AcquireCellAdmissionClaim(intent);
  ASSERT_TRUE(acquired.ok()) << acquired.error().message;
  EXPECT_EQ(acquired.value().intent, intent);
  EXPECT_EQ(checks, 3U);
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(intent).value(), acquired.value());
  EXPECT_EQ(checks, 3U);
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  EXPECT_FALSE(owner
                   .AcquireCellAdmissionClaim(
                       Intent(CellAdmissionClaimKindDTO::kComponentLifecycle,
                              owner.GetAuthoringAdmission().admission_generation, "ordinary"))
                   .ok());
  EXPECT_TRUE(owner.ReleaseCellAdmissionClaim(Release(acquired.value())).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);

  AdmissionJournal closed_journal;
  KernelCellAdmissionOwner closed{
      RequestOwnerEpoch("kernel.closed"), 16U, clock, closed_journal, {}};
  EXPECT_FALSE(closed
                   .AcquireCellAdmissionClaim(
                       RouteRecoveryIntent(closed.GetAuthoringAdmission().admission_generation))
                   .ok());
}

TEST(KernelCellAdmissionOwner, HeldManualEvidenceCannotImpersonateAnotherOwnerOrParent) {
  auto intent = Intent(CellAdmissionClaimKindDTO::kManualOperation, AdmissionGeneration(1U));
  intent.scope.resources = {ResourceId("arm.joint")};
  intent.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
      intent.owner_role, intent.owner_epoch, intent.owner_transaction_id, ContentDigest(kDigest),
      CellAdmissionClaimGeneration(1U)};
  ASSERT_TRUE(ValidateCellAdmissionClaimIntent(intent).ok());
  for (int change = 0; change < 6; ++change) {
    auto bad = intent;
    auto& evidence = *bad.intervention_evidence;
    if (change == 0) evidence.offering_owner_role = CellAdmissionOwnerRoleDTO::kProgramRunner;
    if (change == 1) evidence.offering_owner_epoch = RequestOwnerEpoch("other.owner");
    if (change == 2) evidence.offer_transaction_id = "other.operation";
    if (change == 3) evidence.parent_program_cycle_claim_generation.reset();
    if (change == 4) bad.scope.resources.clear();
    if (change == 5) {
      bad.claim_kind = CellAdmissionClaimKindDTO::kAuthoringMutation;
      bad.owner_role = CellAdmissionOwnerRoleDTO::kAuthoringOwner;
    }
    EXPECT_FALSE(ValidateCellAdmissionClaimIntent(bad).ok()) << change;
  }
}

TEST(KernelCellAdmissionOwner, HeldManualClaimNeedsCurrentOfferAndRetainsProgramHold) {
  FixedClock clock;
  AdmissionJournal journal;
  bool offer_current = false;
  unsigned checks = 0U;
  KernelCellAdmissionOwner owner{
      RequestOwnerEpoch("kernel.1"),
      16U,
      clock,
      journal,
      {},
      AdmissionGeneration(0U),
      [&](const auto& intent, const auto& parent) {
        ++checks;
        EXPECT_TRUE(parent.has_value());
        if (!parent) return Result<void>::Err(ErrorCode::kInvalidStateTransition);
        EXPECT_EQ(parent_program_cycle_claim(intent), parent->claim_generation);
        return offer_current ? Result<void>::Ok()
                             : Result<void>::Err(ErrorCode::kInvalidStateTransition);
      },
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      {},
      {},
      {},
      EmptyLivenessSet()};
  auto ready = Ready();
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  EXPECT_TRUE(owner.CheckManualCellAdmissionReadiness(std::nullopt).ok());
  auto program = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                        owner.GetAuthoringAdmission().admission_generation);
  program.owner_epoch = RequestOwnerEpoch("program.owner");
  program.owner_transaction_id = "run.1";
  const auto parent = owner.AcquireCellAdmissionClaim(program);
  ASSERT_TRUE(parent.ok());
  EXPECT_FALSE(owner.CheckManualCellAdmissionReadiness(std::nullopt).ok());
  EXPECT_FALSE(owner.CheckManualCellAdmissionReadiness(parent.value().claim_generation).ok());
  auto manual = Intent(CellAdmissionClaimKindDTO::kManualOperation,
                       owner.GetAuthoringAdmission().admission_generation, "manual.1");
  manual.scope.resources = {ResourceId("arm.joint")};
  manual.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
      manual.owner_role, manual.owner_epoch, manual.owner_transaction_id, ContentDigest(kDigest),
      parent.value().claim_generation};
  // A current Idle predecessor is insufficient for this handover.
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(manual).ok());
  EXPECT_EQ(checks, 0U);
  ready.program_cycle_status =
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 2U,
                            ProgramCycleStateDTO::kRunning,     ProgramRunId("run.1"),
                            parent.value().claim_generation,    std::nullopt};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  manual.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  EXPECT_TRUE(owner.CheckManualCellAdmissionReadiness(parent.value().claim_generation).ok());
  const auto before = journal.sequence.load();
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(manual).ok());
  EXPECT_EQ(checks, 1U);
  EXPECT_EQ(journal.sequence.load(), before);
  for (int stale = 0; stale < 7; ++stale) {
    auto unavailable = ready;
    if (stale == 0) unavailable.program_cycle_status_current = false;
    if (stale == 1) unavailable.required_component_status_current = false;
    if (stale == 2) unavailable.required_component_current_compatible_ready_converged = 1U;
    if (stale == 3) unavailable.deployment_converged = false;
    if (stale == 4) unavailable.blocking_fault_count = 1U;
    if (stale == 5) unavailable.unresolved_owner_count = 1U;
    if (stale == 6) unavailable.unsettled_effect_count = 1U;
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(unavailable).ok());
    manual.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
    EXPECT_FALSE(owner.CheckManualCellAdmissionReadiness(parent.value().claim_generation).ok());
    EXPECT_FALSE(owner.AcquireCellAdmissionClaim(manual).ok()) << stale;
    EXPECT_EQ(checks, 1U);
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  }
  manual.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  offer_current = true;
  const auto acquired = owner.AcquireCellAdmissionClaim(manual);
  ASSERT_TRUE(acquired.ok()) << acquired.error().message;
  EXPECT_FALSE(owner.CheckManualCellAdmissionReadiness(parent.value().claim_generation).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  EXPECT_EQ(owner.unsettled_program_child_nodes(parent.value().claim_generation),
            std::vector<std::string>{""});
  EXPECT_FALSE(owner.ReleaseCellAdmissionClaim(Release(parent.value())).ok());

  // Replay only observes the admitted transaction despite subsequent loss.
  offer_current = false;
  ready.program_cycle_status_current = false;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(manual).value(), acquired.value());
  EXPECT_EQ(checks, 2U);
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(acquired.value())).ok());
  EXPECT_TRUE(owner.unsettled_program_child_nodes(parent.value().claim_generation).empty());
  EXPECT_EQ(owner.held_claim(CellAdmissionClaimKindDTO::kProgramCycle), parent.value());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);

  manual.request_id = RequestId("manual.2");
  manual.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(manual).ok());
  EXPECT_EQ(checks, 2U);  // Stale cycle cannot reach the offer validator.
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

TEST(KernelCellAdmissionOwner, LifecycleVersusProgramOrManualClaimHasOneWinner) {
  for (const auto competing_kind : {CellAdmissionClaimKindDTO::kProgramCycle,
                                    CellAdmissionClaimKindDTO::kManualOperation}) {
    FixedClock clock;
    AdmissionJournal journal;
    auto owner = Owner(clock, journal);
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    const auto generation = owner.GetAuthoringAdmission().admission_generation;
    std::barrier start(3);
    std::atomic<int> accepted{};
    const auto acquire = [&](CellAdmissionClaimKindDTO kind, const char* request) {
      start.arrive_and_wait();
      if (owner.AcquireCellAdmissionClaim(Intent(kind, generation, request)).ok()) {
        ++accepted;
      }
    };
    std::thread lifecycle(acquire, CellAdmissionClaimKindDTO::kComponentLifecycle,
                          "request.lifecycle");
    std::thread competing(acquire, competing_kind, "request.competing");
    start.arrive_and_wait();
    lifecycle.join();
    competing.join();
    EXPECT_EQ(accepted.load(), 1) << static_cast<int>(competing_kind);
  }
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

TEST(KernelCellAdmissionOwner, ParentReconciliationRequiresEveryHeldChildInTheSameAtomicBatch) {
  FixedClock clock;
  AdmissionJournal journal;
  const CellAdmissionClaimDTO parent{
      Intent(CellAdmissionClaimKindDTO::kProgramCycle, AdmissionGeneration(1U)),
      CellAdmissionClaimGeneration(1U), AdmissionGeneration(1U), 41};
  auto child_intent =
      Intent(CellAdmissionClaimKindDTO::kInterventionOrRecovery, AdmissionGeneration(2U), "child");
  child_intent.intervention_evidence->parent_program_cycle_claim_generation =
      parent.claim_generation;
  const CellAdmissionClaimDTO child{child_intent, CellAdmissionClaimGeneration(2U),
                                    AdmissionGeneration(2U), 42};
  const std::vector<OperationJournalRecoveryDTO::RecoveredCellAdmissionClaimDTO> recovery{
      {parent, {}, {}}, {child, {}, {}}};
  for (const bool settle_child : {false, true}) {
    const std::vector<CellAdmissionCurrentOwnerEpochDTO> successors{
        {CellAdmissionOwnerRoleDTO::kProgramRunner, RequestOwnerEpoch("owner.2")},
        {CellAdmissionOwnerRoleDTO::kInterventionOrRecoveryOwner,
         RequestOwnerEpoch(settle_child ? "owner.2" : "owner.1")}};
    KernelCellAdmissionOwner owner(
        RequestOwnerEpoch("kernel.2"), 4U, clock, journal, recovery, AdmissionGeneration(3U), {},
        {}, [settle_child](const auto& held, const auto&, const auto& reconcile) {
          CellAdmissionReconcileValidationDTO result{reconcile.current_owner_epochs, {}};
          for (const auto& claim : held) {
            if (claim.intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle || settle_child)
              result.settlements.push_back({claim, claim.intent.owner_role,
                                            RequestOwnerEpoch("owner.2"),
                                            CellAdmissionSettlementKindDTO::kReconciled,
                                            reconcile.reconciliation_evidence_digest});
          }
          return Result<CellAdmissionReconcileValidationDTO>::Ok(std::move(result));
        });
    const auto before = journal.sequence.load();
    const CellAdmissionReconcileDTO reconcile{
        RequestOwnerEpoch("kernel.2"), {parent, child}, successors, ContentDigest(kDigest)};
    const auto result = owner.ReconcileCellAdmissionClaims(reconcile);
    ASSERT_EQ(result.ok(), settle_child);
    if (!settle_child) {
      EXPECT_EQ(result.error().details.at("reason"), "reconcile_parent_child_unsettled");
      EXPECT_EQ(journal.sequence.load(), before);
      EXPECT_EQ(owner.held_claim(CellAdmissionClaimKindDTO::kProgramCycle), parent);
      EXPECT_EQ(owner.held_claim(CellAdmissionClaimKindDTO::kInterventionOrRecovery), child);
    } else {
      EXPECT_FALSE(owner.has_unresolved_claims());
      const auto after = journal.sequence.load();
      EXPECT_TRUE(owner.ReconcileCellAdmissionClaims(reconcile).ok());
      EXPECT_EQ(journal.sequence.load(), after);
    }
  }
}

TEST(KernelCellAdmissionOwner, ExactManualReconciliationReplayDoesNotTouchSuccessorWork) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto intent = Intent(CellAdmissionClaimKindDTO::kManualOperation, AdmissionGeneration(7U));
  const CellAdmissionClaimDTO claim{intent, CellAdmissionClaimGeneration(4U),
                                    AdmissionGeneration(7U), 41};
  int validations = 0;
  KernelCellAdmissionOwner owner(
      RequestOwnerEpoch("kernel.2"), 4U, clock, journal, {{claim, {}, {}}}, AdmissionGeneration(8U),
      {}, {},
      [&](const auto& held, const auto&, const auto& request) {
        ++validations;
        return Result<CellAdmissionReconcileValidationDTO>::Ok(
            {request.current_owner_epochs,
             {{held.front(), CellAdmissionOwnerRoleDTO::kManualOperationOwner,
               RequestOwnerEpoch("kernel.2"), CellAdmissionSettlementKindDTO::kReconciled,
               request.reconciliation_evidence_digest}}});
      },
      {}, {{CellAdmissionOwnerRoleDTO::kManualOperationOwner, intent.owner_epoch}},
      EmptyLivenessSet());
  const auto request = owner.manual_owner_reconciliation_request();
  ASSERT_TRUE(request.ok() && request.value().has_value());
  ASSERT_TRUE(owner.ReconcileCellAdmissionClaims(*request.value()).ok());
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto next = Intent(CellAdmissionClaimKindDTO::kManualOperation,
                     owner.GetAuthoringAdmission().admission_generation, "successor.manual");
  next.owner_epoch = RequestOwnerEpoch("kernel.2");
  const auto acquired = owner.AcquireCellAdmissionClaim(next);
  ASSERT_TRUE(acquired.ok());
  const auto before = owner.GetAuthoringAdmission();
  const auto writes = journal.sequence.load();
  const auto replay = owner.ReconcileCellAdmissionClaims(*request.value());
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value(), before);
  EXPECT_FALSE(replay.value().authoring_allowed);
  EXPECT_EQ(journal.sequence.load(), writes);
  EXPECT_EQ(validations, 1);
  EXPECT_EQ(owner.held_claim(CellAdmissionClaimKindDTO::kManualOperation), acquired.value());
  auto changed = *request.value();
  changed.retained_claims.clear();
  EXPECT_FALSE(owner.ReconcileCellAdmissionClaims(changed).ok());
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
