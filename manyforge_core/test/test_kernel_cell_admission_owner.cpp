#include <manyforge_core/testing/request_key_fixture.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <manyforge_core/kernel_cell_admission_owner.hpp>
#include <mutex>
#include <span>
#include <thread>
#include <unistd.h>

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
  Result<void> BeginStart(ApplicationStartId, std::int64_t, std::size_t) override {
    return Result<void>::Ok();
  }
  JournalBatchResult WriteBatch(std::span<const JournalRecord>, std::uint64_t) override {
    return {};
  }
  // The owner's claim-record poster: keeps the posted records and counts each
  // as one recorded write.
  CellAdmissionJournalPoster Poster() {
    return [this](std::span<JournalRecord> records) {
      std::lock_guard lock(posted_mutex);
      for (auto& record : records) posted.push_back(std::move(record));
      sequence += records.size();
    };
  }
  std::vector<JournalRecord> Posted() {
    std::lock_guard lock(posted_mutex);
    return posted;
  }

  std::atomic<std::uint64_t> sequence{};
  std::mutex posted_mutex;
  std::vector<JournalRecord> posted;
};

CellAdmissionReadinessSnapshotDTO Ready() {
  return CellAdmissionReadinessSnapshotDTO{
      true,
      ProgramCycleStatusDTO{1U,
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
  return {TestRequestKey("reset.1"), ContentDigest(kDigest), true, false, std::nullopt};
}

ApplicationRole Role(CellAdmissionClaimKindDTO kind) {
  switch (kind) {
    case CellAdmissionClaimKindDTO::kProgramCycle:
    case CellAdmissionClaimKindDTO::kProgramReset:
      return ApplicationRole::kProgramRunner;
    case CellAdmissionClaimKindDTO::kAuthoringMutation:
      return ApplicationRole::kAuthoringOwner;
    case CellAdmissionClaimKindDTO::kManagerApply:
      return ApplicationRole::kManager;
    case CellAdmissionClaimKindDTO::kAuthoredScene:
      return ApplicationRole::kAuthoredSceneOwner;
    case CellAdmissionClaimKindDTO::kManualOperation:
      return ApplicationRole::kManualOperationOwner;
    case CellAdmissionClaimKindDTO::kInterventionOrRecovery:
      return ApplicationRole::kInterventionOrRecoveryOwner;
  }
  return ApplicationRole::kAuthoringOwner;
}

// The owner's typed transaction number for `kind`: present only for a Program
// cycle, a Manager Apply and a manual operation.
std::optional<CellAdmissionOwnerTransactionId> TransactionId(CellAdmissionClaimKindDTO kind,
                                                             std::uint64_t number = 105U) {
  switch (kind) {
    case CellAdmissionClaimKindDTO::kProgramCycle:
      return CellAdmissionOwnerTransactionId(ProgramRunNumber(number));
    case CellAdmissionClaimKindDTO::kManagerApply:
      return CellAdmissionOwnerTransactionId(ApplyId(number));
    case CellAdmissionClaimKindDTO::kManualOperation:
      return CellAdmissionOwnerTransactionId(OperationNumber(number));
    default:
      return std::nullopt;
  }
}

CellAdmissionClaimIntentDTO Intent(CellAdmissionClaimKindDTO kind, AdmissionGeneration generation,
                                   std::string request = "request.1") {
  CellAdmissionClaimIntentDTO intent{kind,
                                     Role(kind),
                                     TransactionId(kind),
                                     TestRequestKey(std::move(request)),
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
        ApplicationRole::kProgramRunner,
        CellAdmissionOwnerTransactionId(ProgramRunNumber(1U)), ContentDigest(kDigest),
        std::nullopt};
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
      claim.intent.owner_transaction_id,
      claim.intent.request_key,
      claim.acquired_admission_generation,
      claim.claim_generation,
      CellAdmissionSettlementEvidenceDTO{settlement, ContentDigest(kDigest)}};
}

ManagerLivenessRequiredSetDTO EmptyLivenessSet() {
  ManagerLivenessRequiredSetDTO result{CellId("cell.1"),
                                       RetainedBundleId("bundle.1"),
                                       ContentDigest(kDigest),
                                       RuntimeUnitId("manager"),
                                       {}};
  result.required_runtime_set_digest = ComputeRequiredRuntimeSetDigest(result.required_receivers);
  return result;
}

KernelCellAdmissionOwner Owner(FixedClock& clock, AdmissionJournal& journal,
                               std::size_t capacity = 16U,
                               CellAdmissionProgramNodeValidator program_node_validator = {}) {
  return KernelCellAdmissionOwner(capacity, clock, journal.Poster(),
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&) { return Result<void>::Ok(); }, EmptyLivenessSet(),
      std::move(program_node_validator));
}

// A4: the expected admission generation is claim evidence only. A Disable
// moves the generation; a caller that still names the pre-Disable generation
// is admitted on the current facts, which alone can refuse.
TEST(KernelCellAdmissionOwner, OldGenerationAfterDisableIsAcceptedOnCurrentFacts) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).value().authoring_allowed);
  const auto observed = owner.GetAuthoringAdmission().admission_generation;
  const auto disable = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, observed, "disable"));
  ASSERT_TRUE(disable.ok());
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(disable.value())).ok());
  // Manager publishes the reduced required set after the Disable.
  auto disabled = Ready();
  disabled.required_component_total = 1U;
  disabled.required_component_current_compatible_ready_converged = 1U;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(disabled).value().authoring_allowed);
  ASSERT_GT(owner.GetAuthoringAdmission().admission_generation.value(), observed.value());

  // Preview (authoring), Apply, Execute (manual) and Run, each still naming
  // the pre-Disable generation.
  const std::vector<std::pair<CellAdmissionClaimKindDTO, std::string>> requests{
      {CellAdmissionClaimKindDTO::kAuthoringMutation, "preview"},
      {CellAdmissionClaimKindDTO::kManagerApply, "apply"},
      {CellAdmissionClaimKindDTO::kManualOperation, "execute"},
      {CellAdmissionClaimKindDTO::kProgramCycle, "run"}};
  for (const auto& [kind, request] : requests) {
    auto intent = Intent(kind, observed, request);
    const auto accepted = owner.AcquireCellAdmissionClaim(intent);
    ASSERT_TRUE(accepted.ok()) << request << ": " << accepted.error().message;
    EXPECT_EQ(accepted.value().intent.expected_admission_generation, observed);
    EXPECT_GT(accepted.value().acquired_admission_generation.value(), observed.value());
    // Exact replay still returns the recorded claim.
    EXPECT_EQ(owner.AcquireCellAdmissionClaim(intent).value(), accepted.value());
    if (kind == CellAdmissionClaimKindDTO::kAuthoringMutation) {
      // A held claim is a real admission fact: a second caller is refused
      // whatever generation it names.
      for (const auto generation : {observed, owner.GetAuthoringAdmission().admission_generation}) {
        const auto busy = owner.AcquireCellAdmissionClaim(
            Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, generation, "preview.busy"));
        ASSERT_FALSE(busy.ok());
        EXPECT_EQ(busy.error().code, "authoring_not_ready");
      }
    }
    ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(accepted.value())).ok());
  }
}

TEST(KernelCellAdmissionOwner, ConfigurationApplyRetainsTerminalMarkersUntilFreshSuccess) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto old_intent = Intent(CellAdmissionClaimKindDTO::kManagerApply,
                           owner.GetAuthoringAdmission().admission_generation, "old.failed");
  const auto old = owner.AcquireCellAdmissionClaim(old_intent);
  ASSERT_TRUE(old.ok());
  auto fresh = old_intent;
  fresh.request_key = TestRequestKey("fresh.apply");
  fresh.owner_transaction_id = ApplyId(106U);
  fresh.expected_admission_generation = owner.GetAuthoringAdmission().admission_generation;
  const auto before = journal.sequence.load();
  EXPECT_FALSE(owner.AcquireConfigurationApplyClaim(fresh, {}).ok());
  EXPECT_FALSE(
      owner
          .AcquireConfigurationApplyClaim(fresh,
                                          [](const auto&, auto, const auto&) {
                                            return Result<void>::Err(ErrorDTO{
                                                "apply_live", "old Apply has no terminal", {}});
                                          })
          .ok());
  EXPECT_EQ(journal.sequence.load(), before);
  int checked = 0;
  const auto acquired = owner.AcquireConfigurationApplyClaim(
      fresh, [&](const auto& intent, auto held, const auto&) {
        ++checked;
        EXPECT_EQ(intent, fresh);
        EXPECT_EQ(held.size(), 1U);
        if (!held.empty()) {
          EXPECT_EQ(held.front(), old.value());
        }
        return Result<void>::Ok();
      });
  ASSERT_TRUE(acquired.ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  const auto replay =
      owner.AcquireConfigurationApplyClaim(fresh, [&](const auto&, auto, const auto&) {
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

TEST(KernelCellAdmissionOwner, ConfigurationApplyCannotBypassForeignClaims) {
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
  int checked = 0;
  const auto validate = [&](const auto&, auto, const auto&) {
    ++checked;
    return Result<void>::Ok();
  };
  EXPECT_FALSE(owner.AcquireConfigurationApplyClaim(intent, validate).ok());
  EXPECT_EQ(checked, 0);
}

// A configuration Apply and its success release take effect in memory; their
// rows follow and cannot refuse them.
TEST(KernelCellAdmissionOwner, ConfigurationApplyIsNeverRefusedForStorage) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const auto old = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kManagerApply,
             owner.GetAuthoringAdmission().admission_generation, "old.apply"));
  ASSERT_TRUE(old.ok());
  auto fresh_intent = Intent(CellAdmissionClaimKindDTO::kManagerApply,
                             owner.GetAuthoringAdmission().admission_generation, "fresh.apply");
  fresh_intent.owner_transaction_id = ApplyId(106U);
  const auto fresh = owner.AcquireConfigurationApplyClaim(
      fresh_intent, [](const auto&, auto, const auto&) { return Result<void>::Ok(); });
  ASSERT_TRUE(fresh.ok());
  EXPECT_EQ(fresh.value().accepted_at_ns, 42);
  const auto generation = owner.GetAuthoringAdmission().admission_generation;
  const auto settled = owner.ReleaseTerminalApplyClaimsAfterSuccess(
      fresh.value(), Release(fresh.value()),
      [](const auto&, const auto&, auto) { return Result<void>::Ok(); });
  ASSERT_TRUE(settled.ok());
  EXPECT_EQ(settled.value().admission_generation.value(), generation.value() + 1U);
  EXPECT_EQ(owner.held_claims(CellAdmissionClaimKindDTO::kManagerApply),
            std::vector<CellAdmissionClaimDTO>{fresh.value()});

  const auto posted = journal.Posted();
  ASSERT_EQ(posted.size(), 3U);
  EXPECT_EQ(std::get<JournalCellClaimAcquired>(posted[0].payload).claim, old.value());
  EXPECT_EQ(std::get<JournalCellClaimAcquired>(posted[1].payload).claim, fresh.value());
  const auto& released = std::get<JournalCellClaimReleased>(posted[2].payload);
  EXPECT_EQ(released.claim, old.value());
  EXPECT_EQ(released.release.claim_generation, old.value().claim_generation);
  EXPECT_EQ(released.release.request_key, old.value().intent.request_key);
  EXPECT_EQ(released.release.settlement, Release(fresh.value()).settlement);
  EXPECT_EQ(released.released_at_ns, 42);
  EXPECT_STREQ(posted[2].write_kind, "cell_claim_released");
}

// Each claim row is posted with both owner locks free, after the claim change
// is visible in memory. A replay or a refusal posts nothing.
TEST(KernelCellAdmissionOwner, ClaimRowsArePostedAfterTheOwnerUnlocks) {
  FixedClock clock;
  AdmissionJournal journal;
  KernelCellAdmissionOwner* self = nullptr;
  std::vector<bool> held_at_post;
  auto post = journal.Poster();
  KernelCellAdmissionOwner owner(
      16U, clock,
      [&](std::span<JournalRecord> records) {
        // Takes both owner locks: a post under either would self-deadlock.
        EXPECT_TRUE(self->SetRouteRecoveryPending(false).ok());
        held_at_post.push_back(self->has_unresolved_claims());
        post(records);
      },
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&) { return Result<void>::Ok(); }, EmptyLivenessSet());
  self = &owner;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const auto intent = Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                             owner.GetAuthoringAdmission().admission_generation);
  const auto claim = owner.AcquireCellAdmissionClaim(intent);
  ASSERT_TRUE(claim.ok());
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(intent).value(), claim.value());
  const auto release = Release(claim.value());
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(release).ok());
  EXPECT_EQ(owner.ReleaseCellAdmissionClaim(release).error().code, "stale_generation");
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(intent).error().code, "request_result_expired");

  EXPECT_EQ(held_at_post, (std::vector<bool>{true, false}));
  const auto posted = journal.Posted();
  ASSERT_EQ(posted.size(), 2U);
  EXPECT_STREQ(posted[0].write_kind, "cell_claim_acquired");
  EXPECT_EQ(std::get<JournalCellClaimAcquired>(posted[0].payload).claim, claim.value());
  const auto& released = std::get<JournalCellClaimReleased>(posted[1].payload);
  EXPECT_EQ(released.claim, claim.value());
  EXPECT_EQ(released.release, release);
  EXPECT_EQ(released.released_at_ns, 42);
}

TEST(KernelCellAdmissionOwner, RouteRecoveryBarrierSurvivesReadyUpdatesAndClaimSettlement) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).value().authoring_allowed);
  const auto apply = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kManagerApply,
             owner.GetAuthoringAdmission().admission_generation, "manager.apply"));
  ASSERT_TRUE(apply.ok());
  EXPECT_TRUE(owner.has_unresolved_claims());
  EXPECT_FALSE(owner.has_unresolved_claims_except(ApplicationRole::kManager));
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(apply.value())).ok());
  auto authoring = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
             owner.GetAuthoringAdmission().admission_generation, "apply"));
  ASSERT_TRUE(authoring.ok());
  EXPECT_TRUE(owner.has_unresolved_claims_except(ApplicationRole::kManager));
  ASSERT_TRUE(owner.SetRouteRecoveryPending(true).ok());
  EXPECT_FALSE(
      owner.ReleaseCellAdmissionClaim(Release(authoring.value())).value().authoring_allowed);
  EXPECT_FALSE(owner.UpdateCellAdmissionReadiness(Ready()).value().authoring_allowed);
  for (auto kind :
       {CellAdmissionClaimKindDTO::kProgramCycle, CellAdmissionClaimKindDTO::kAuthoringMutation,
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
  // The generation advances in memory only: a route-recovery change posts
  // no journal record.
  const auto before = journal.sequence.load();
  ASSERT_TRUE(owner.SetRouteRecoveryPending(true).ok());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  ASSERT_TRUE(owner.SetRouteRecoveryPending(false).ok());
  EXPECT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);
  EXPECT_EQ(journal.sequence.load(), before);
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
  });
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  auto parent_intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                              owner.GetAuthoringAdmission().admission_generation);
  parent_intent.owner_transaction_id = ProgramRunNumber(1U);
  const auto parent = owner.AcquireCellAdmissionClaim(parent_intent);
  ASSERT_TRUE(parent.ok());
  ready.program_cycle_status =
      ProgramCycleStatusDTO{2U,
                            ProgramCycleStateDTO::kRunning,     ProgramRunNumber(1U),
                            parent.value().claim_generation,    std::nullopt};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  auto child = Intent(CellAdmissionClaimKindDTO::kAuthoredScene,
                      owner.GetAuthoringAdmission().admission_generation, "scene.child");
  child.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{ProgramRunNumber(1U),
                                                                parent.value().claim_generation,
                                                                "attach",
                                                                NodeAttemptNumber(1U),
                                                                "scene.carried_attachment"};
  const auto scene = owner.AcquireCellAdmissionClaim(child);
  ASSERT_TRUE(scene.ok()) << scene.error().message;
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  EXPECT_EQ(owner.unsettled_program_child_nodes(parent.value().claim_generation),
            std::vector<std::string>{"attach"});
  held = true;
  const auto before = journal.sequence.load();
  ASSERT_EQ(owner.AcquireCellAdmissionClaim(child).value(), scene.value());
  EXPECT_EQ(checks, 1U);  // Refusal and exact replay never re-enter effect admission.
  EXPECT_EQ(journal.sequence.load(), before);
  auto late = child;
  late.request_key = TestRequestKey("scene.late");
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
  // The runner's current cycle status travels in the authorization.
  const ProgramResetSceneAuthorizationDTO reset{ProgramRunNumber(1U), parent.value().claim_generation,
      ContentDigest(kDigest), std::nullopt, *ready.program_cycle_status};
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
    RuntimeUnitId manager = RuntimeUnitId("manager")) {
  ManagerLivenessRequiredSetDTO result{CellId("cell.1"), RetainedBundleId("bundle.1"),
                                       ContentDigest(kDigest), std::move(manager),
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
                                  set.required_receivers.at(receiver).runtime_policy_id,
                                  set.required_receivers.at(receiver).runtime_policy_revision,
                                  sequence,
                                  sequence,
                                  state,
                                  fenced,
                                  std::nullopt};
  return {status, set.active_bundle_id, set.required_runtime_set_digest};
}

KernelCellAdmissionOwner LivenessOwner(FixedClock& clock, AdmissionJournal& journal,
                                       const ManagerLivenessRequiredSetDTO& set,
                                       CellAdmissionProgramNodeValidator program_node_validator = {}) {
  return KernelCellAdmissionOwner(16U, clock, journal.Poster(),
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      [](const auto&, const auto&) { return Result<void>::Ok(); }, set,
      std::move(program_node_validator));
}

TEST(KernelCellAdmissionOwner, ProgramNodeDependencyRefusalReportsCurrentReadinessFacts) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto set = LivenessSet();
  auto owner = LivenessOwner(clock, journal, set, [](const auto&) {
    return Result<void>::Ok();
  });
  auto ready = Ready();
  ready.program_cycle_status = ProgramCycleStatusDTO{1U, ProgramCycleStateDTO::kStarting,
      ProgramRunNumber(1U), std::nullopt, std::nullopt};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  for (std::size_t receiver = 0; receiver < set.required_receivers.size(); ++receiver) {
    ASSERT_TRUE(owner.Publish(LivenessObservation(set, receiver, 1U,
                                                  ManagerLivenessStateDTO::kCurrent)).ok());
  }

  auto parent_intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                              owner.GetAuthoringAdmission().admission_generation, "1");
  parent_intent.owner_transaction_id = ProgramRunNumber(1U);
  const auto parent = owner.AcquireCellAdmissionClaim(parent_intent);
  ASSERT_TRUE(parent.ok());

  ready.program_cycle_status = ProgramCycleStatusDTO{2U, ProgramCycleStateDTO::kRunning,
      ProgramRunNumber(1U), parent.value().claim_generation, std::nullopt};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  ASSERT_TRUE(owner.Publish(LivenessObservation(set, 0U, 2U,
                                                ManagerLivenessStateDTO::kUnreconciled, true)).ok());
  ready.program_cycle_status = ProgramCycleStatusDTO{2U, ProgramCycleStateDTO::kRunning,
      ProgramRunNumber(1U), parent.value().claim_generation, std::nullopt};
  ready.deployment_status_current = false;
  ready.deployment_converged = false;
  ready.blocking_fault_count = 2U;
  ready.unresolved_owner_count = 3U;
  ready.unsettled_effect_count = 4U;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());

  auto child = Intent(CellAdmissionClaimKindDTO::kAuthoredScene,
                      owner.GetAuthoringAdmission().admission_generation, "scene.child");
  child.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{
      ProgramRunNumber(1U), parent.value().claim_generation, "attach", NodeAttemptNumber(1U),
      "scene.carried_attachment"};
  const auto sequence_before = journal.sequence.load();
  const auto refused = owner.AcquireCellAdmissionClaim(child);

  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "authoring_not_ready");
  EXPECT_EQ(refused.error().details.at("reason"), "program_node_dependencies_not_current");
  EXPECT_EQ(refused.error().details.at("manager_liveness_required_set_present"), "true");
  EXPECT_EQ(refused.error().details.at("manager_liveness_current_unfenced_count"), "1");
  EXPECT_EQ(refused.error().details.at("manager_liveness_required_count"), "2");
  EXPECT_EQ(refused.error().details.at("deployment_status_current"), "false");
  EXPECT_EQ(refused.error().details.at("deployment_converged"), "false");
  EXPECT_EQ(refused.error().details.at("blocking_fault_count"), "2");
  EXPECT_EQ(refused.error().details.at("unresolved_owner_count"), "3");
  EXPECT_EQ(refused.error().details.at("unsettled_effect_count"), "4");
  EXPECT_NE(refused.error().message.find("manager_liveness_current_unfenced_count=1"),
            std::string::npos);
  EXPECT_NE(refused.error().message.find("unsettled_effect_count=4"), std::string::npos);
  EXPECT_EQ(journal.sequence.load(), sequence_before);
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);

  // S1: a child naming the older, pre-Run generation is refused by the same
  // current fact; the generation it names never decides.
  const auto old_generation = parent_intent.expected_admission_generation;
  EXPECT_LT(old_generation.value(), owner.GetAuthoringAdmission().admission_generation.value());
  auto old_child = Intent(CellAdmissionClaimKindDTO::kAuthoredScene, old_generation,
                          "scene.child.old");
  old_child.program_node_evidence = child.program_node_evidence;
  const auto old_refused = owner.AcquireCellAdmissionClaim(old_child);
  ASSERT_FALSE(old_refused.ok());
  EXPECT_EQ(old_refused.error().code, "authoring_not_ready");
  EXPECT_EQ(old_refused.error().details.at("reason"), "program_node_dependencies_not_current");
  EXPECT_EQ(journal.sequence.load(), sequence_before);
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
  acquiring.program_cycle_status = ProgramCycleStatusDTO{2U,           ProgramCycleStateDTO::kStarting,
      ProgramRunNumber(12U),    std::nullopt, std::nullopt};
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
            ProgramCycleStatusDTO{2U,
                                  ProgramCycleStateDTO::kTerminalAwaitingReset,
                                  ProgramRunNumber(1U),
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
            ProgramCycleStatusDTO{2U,
                                  ProgramCycleStateDTO::kRunning,     ProgramRunNumber(1U),
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
  starting.program_cycle_status = ProgramCycleStatusDTO{1U,           ProgramCycleStateDTO::kStarting,
      ProgramRunNumber(105U), std::nullopt, std::nullopt};
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
  EXPECT_EQ(cycle.value().intent.owner_transaction_id,
            CellAdmissionOwnerTransactionId(ProgramRunNumber(105U)));
}

TEST(KernelCellAdmissionOwner,
     StartingProgramCycleExceptionIsExactAndFailsClosedForEveryOtherCase) {
  const auto rejected = [](auto mutate_readiness, auto mutate_intent) {
    FixedClock clock;
    AdmissionJournal journal;
    auto owner = Owner(clock, journal);
    auto starting = Ready();
    starting.program_cycle_status = ProgramCycleStatusDTO{1U,           ProgramCycleStateDTO::kStarting,
        ProgramRunNumber(105U), std::nullopt, std::nullopt};
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
           [](auto& intent) { intent.owner_transaction_id = ProgramRunNumber(145U); });
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

TEST(KernelCellAdmissionOwner, StartingClaimNeverBypassesANonCurrentManagerAndHasOneConcurrentWinner) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto set = LivenessSet();
  auto owner = LivenessOwner(clock, journal, set);
  auto starting = Ready();
  starting.program_cycle_status = ProgramCycleStatusDTO{1U,           ProgramCycleStateDTO::kStarting,
      ProgramRunNumber(105U), std::nullopt, std::nullopt};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(starting).ok());
  for (std::size_t receiver = 0; receiver < set.required_receivers.size(); ++receiver) {
    ASSERT_TRUE(
        owner.Publish(LivenessObservation(set, receiver, 1U, ManagerLivenessStateDTO::kCurrent))
            .ok());
  }
  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 2U, ManagerLivenessStateDTO::kUnreconciled, true))
          .ok());
  EXPECT_EQ(owner
                .AcquireCellAdmissionClaim(
                    Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                           owner.GetAuthoringAdmission().admission_generation, "starting.expired"))
                .error()
                .code,
            "authoring_not_ready");

  // A separate healthy owner isolates the actual admission race, with no
  // sleeps and no attempt to reopen a non-current Manager incarnation.
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
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                       owner.GetAuthoringAdmission().admission_generation, "program-reset.claim");
  intent.owner_transaction_id = ProgramRunNumber(12U);
  const auto acquired = owner.AcquireCellAdmissionClaim(intent);
  ASSERT_TRUE(acquired.ok());
  // The authorization carries the runner's current cycle status (still idle).
  ProgramResetSceneAuthorizationDTO authorization{ProgramRunNumber(12U),
      acquired.value().claim_generation, ContentDigest(kDigest), std::nullopt,
      *resetting.program_cycle_status};

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
      ProgramCycleStatusDTO{2U,
                            ProgramCycleStateDTO::kResetting,   ProgramRunNumber(12U),
                            acquired.value().claim_generation,  ContentDigest(kDigest), AcceptedReset()};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(resetting).ok());
  authorization.program_cycle_status = *resetting.program_cycle_status;
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
  child_intent.request_key = TestRequestKey("program-reset.late-child");
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
  auto owner = Owner(clock, journal);
  ready.program_cycle_status->state = ProgramCycleStateDTO::kResetting;
  ready.program_cycle_status->accepted_reset = AcceptedReset();
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  const auto identity = ProgramResetClaimRequestKey(*ready.program_cycle_status);
  ASSERT_TRUE(identity.ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
                       owner.GetAuthoringAdmission().admission_generation,
                       "reset.claim");
  intent.request_key = identity.value();
  // A Reset claim binds to the accepted Reset by its request key alone.
  auto foreign = intent;
  foreign.request_key = TestRequestKey("foreign.reset.claim");
  const auto carried = *ready.program_cycle_status;
  EXPECT_FALSE(owner.AcquireProgramResetClaim(foreign, carried).ok());
  // A Reset claim without the runner's status is never current.
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(intent).error().details.at("reason"),
            "program_reset_not_current");
  const auto acquired = owner.AcquireProgramResetClaim(intent, carried);
  ASSERT_TRUE(acquired.ok());
  EXPECT_EQ(owner.AcquireProgramResetClaim(intent, carried).value(), acquired.value());
  EXPECT_FALSE(owner.GetAuthoringAdmission().authoring_allowed);
  ready.program_cycle_status->accepted_reset->reset_claim_generation = acquired.value().claim_generation;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  const ProgramResetSceneAuthorizationDTO authorization{
      ProgramRunNumber(0U), CellAdmissionClaimGeneration(0U), ContentDigest(""),
      ProgramResetClaimSceneAuthorizationDTO{
          ready.program_cycle_status->cycle_generation,
          AcceptedReset().request_key,
          AcceptedReset().request_payload_digest, acquired.value().claim_generation},
      *ready.program_cycle_status};
  EXPECT_TRUE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
  auto stale = authorization;
  stale.reset_claim->request_key = TestRequestKey("foreign.reset");
  EXPECT_FALSE(owner.ValidateProgramResetSceneAuthorization(stale).ok());
  stale = authorization;
  stale.program_run_id = ProgramRunNumber(134U);
  EXPECT_FALSE(ValidateProgramResetSceneAuthorization(stale).ok());
  for (const auto kind : {CellAdmissionClaimKindDTO::kProgramCycle,
                          CellAdmissionClaimKindDTO::kAuthoringMutation}) {
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
  const auto identity = ProgramResetClaimRequestKey(status);
  ASSERT_TRUE(identity.ok());
  status.accepted_reset->reset_claim_generation = CellAdmissionClaimGeneration(5U);
  EXPECT_EQ(ProgramResetClaimRequestKey(status).value(), identity.value());
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

// OD-139: every Reset proof (Reset claim, scene and position authorization)
// uses the runner's status carried in the request; a wrong run in that status
// is refused with the same code as when the runner's journal named it.
TEST(KernelCellAdmissionOwner, ResetGrantsUseTheCarriedStatusAndRefuseAWrongRun) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  auto ready = Ready();
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  auto parent_intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
      owner.GetAuthoringAdmission().admission_generation);
  parent_intent.owner_transaction_id = ProgramRunNumber(144U);
  const auto parent = owner.AcquireCellAdmissionClaim(parent_intent);
  ASSERT_TRUE(parent.ok());
  auto accepted = AcceptedReset();
  accepted.reset_robot_position = true;
  ready.program_cycle_status = ProgramCycleStatusDTO{2U,
      ProgramCycleStateDTO::kResetting, ProgramRunNumber(144U),
      parent.value().claim_generation, ContentDigest(kDigest), accepted};
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  auto wrong_run = *ready.program_cycle_status;
  wrong_run.program_run_id = ProgramRunNumber(145U);

  ProgramResetSceneAuthorizationDTO scene{ProgramRunNumber(144U),
      parent.value().claim_generation, ContentDigest(kDigest), std::nullopt,
      *ready.program_cycle_status};
  EXPECT_TRUE(owner.ValidateProgramResetSceneAuthorization(scene).ok());
  scene.program_cycle_status = wrong_run;
  const auto refused_scene = owner.ValidateProgramResetSceneAuthorization(scene);
  ASSERT_FALSE(refused_scene.ok());
  EXPECT_EQ(refused_scene.error().details.at("reason"), "program_reset_status_mismatch");

  ProgramResetPositionAuthorizationDTO position{2U,
      accepted.request_key, accepted.request_payload_digest,
      ProgramRunNumber(144U), parent.value().claim_generation,
      ContentDigest(kDigest), std::nullopt, *ready.program_cycle_status};
  EXPECT_TRUE(owner.ValidateProgramResetPositionAuthorization(position).ok());
  position.program_cycle_status = wrong_run;
  const auto refused_position = owner.ValidateProgramResetPositionAuthorization(position);
  ASSERT_FALSE(refused_position.ok());
  EXPECT_EQ(refused_position.error().details.at("reason"), "program_reset_status_mismatch");

  AdmissionJournal idle_journal;
  auto idle = Owner(clock, idle_journal);
  ready = Ready();
  ready.program_cycle_status->state = ProgramCycleStateDTO::kResetting;
  ready.program_cycle_status->accepted_reset = AcceptedReset();
  ASSERT_TRUE(idle.UpdateCellAdmissionReadiness(ready).ok());
  const auto identity = ProgramResetClaimRequestKey(*ready.program_cycle_status);
  ASSERT_TRUE(identity.ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
      idle.GetAuthoringAdmission().admission_generation, "reset.claim");
  intent.request_key = identity.value();
  auto idle_wrong_run = *ready.program_cycle_status;
  idle_wrong_run.program_run_id = ProgramRunNumber(7U);
  idle_wrong_run.terminal_or_settlement_reference = ContentDigest(kDigest);
  const auto before = idle_journal.sequence.load();
  // The cleanup run is the carried status's own run, and only for a match.
  EXPECT_FALSE(idle.ProgramResetRunForCleanup(intent, idle_wrong_run).value().has_value());
  const auto refused_acquire = idle.AcquireProgramResetClaim(intent, idle_wrong_run);
  ASSERT_FALSE(refused_acquire.ok());
  EXPECT_EQ(refused_acquire.error().details.at("reason"), "program_reset_not_current");
  EXPECT_EQ(idle_journal.sequence.load(), before);
  EXPECT_FALSE(idle.has_unresolved_claims());
  ASSERT_TRUE(idle.AcquireProgramResetClaim(intent, *ready.program_cycle_status).ok());
}

TEST(KernelCellAdmissionOwner, ResetCleanupRunComesFromTheCarriedStatus) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  auto ready = Ready();
  ready.program_cycle_status->state = ProgramCycleStateDTO::kResetting;
  ready.program_cycle_status->accepted_reset = AcceptedReset();
  ready.program_cycle_status->program_run_id = ProgramRunNumber(31U);
  ready.program_cycle_status->terminal_or_settlement_reference = ContentDigest(kDigest);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  const auto identity = ProgramResetClaimRequestKey(*ready.program_cycle_status);
  ASSERT_TRUE(identity.ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
      owner.GetAuthoringAdmission().admission_generation, "reset.claim");
  intent.request_key = identity.value();
  const auto run = owner.ProgramResetRunForCleanup(intent, *ready.program_cycle_status);
  ASSERT_TRUE(run.ok());
  ASSERT_TRUE(run.value().has_value());
  EXPECT_EQ(run.value()->value(), 31U);
  // A relay that is not current refuses before any cleanup, as before.
  auto closed = ready;
  closed.program_cycle_status_current = false;
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(closed).ok());
  EXPECT_EQ(owner.ProgramResetRunForCleanup(intent, *ready.program_cycle_status)
                .error().details.at("reason"),
            "program_reset_status_not_current");
}

TEST(KernelCellAdmissionOwner, ExactResetStatusJoinsCurrentOwnerWithoutWaitingForHeartbeat) {
  FixedClock clock;
  AdmissionJournal journal;
  auto actual = *Ready().program_cycle_status;
  actual.state = ProgramCycleStateDTO::kResetting;
  actual.accepted_reset = AcceptedReset();
  auto owner = Owner(clock, journal);
  // The relayed readiness still shows the cycle idle; the carried status is
  // the runner's newer one.
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const auto identity = ProgramResetClaimRequestKey(actual);
  ASSERT_TRUE(identity.ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
                       owner.GetAuthoringAdmission().admission_generation, "reset.claim");
  intent.request_key = identity.value();
  const auto acquired = owner.AcquireProgramResetClaim(intent, actual);
  ASSERT_TRUE(acquired.ok());
  // An exact replay is independent of the carried status.
  auto changed = actual;
  changed.cycle_generation += 1U;
  ASSERT_TRUE(owner.AcquireProgramResetClaim(intent, changed).ok());
  actual.accepted_reset->reset_claim_generation = acquired.value().claim_generation;
  ProgramResetSceneAuthorizationDTO authorization{
      ProgramRunNumber(0U), CellAdmissionClaimGeneration(0U), ContentDigest(""),
      ProgramResetClaimSceneAuthorizationDTO{actual.cycle_generation,
          actual.accepted_reset->request_key,
          actual.accepted_reset->request_payload_digest, acquired.value().claim_generation},
      actual};
  EXPECT_TRUE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
  authorization.program_cycle_status.cycle_generation += 1U;
  EXPECT_FALSE(owner.ValidateProgramResetSceneAuthorization(authorization).ok());
}

// OD-139: Program-cycle and Program-Reset releases are proven from the
// Kernel's memory alone. The settlement validator (the owner-file reader of
// other claim kinds) is never consulted for them, and the runner's journal
// path, absent or unreadable, is never opened or created.
TEST(KernelCellAdmissionOwner, ProgramReleasesNeedNoOwnerFile) {
  for (const bool unreadable : {false, true}) {
    const auto root = std::filesystem::temp_directory_path() /
        ("manyforge-od139-" + std::to_string(::getpid()) + (unreadable ? "-mode000" : "-absent"));
    std::filesystem::remove_all(root);
    ASSERT_TRUE(std::filesystem::create_directory(root));
    const auto journal_path = root / "program-cycle.sqlite3";
    if (unreadable) {
      { std::ofstream created(journal_path); }
      std::filesystem::permissions(journal_path, std::filesystem::perms::none);
    }
    FixedClock clock;
    AdmissionJournal journal;
    unsigned validator_calls = 0U;
    KernelCellAdmissionOwner owner(16U, clock, journal.Poster(),
        [](const auto&, const auto&) { return Result<void>::Ok(); },
        [&](const auto&, const auto&) {
          ++validator_calls;
          return Result<void>::Err(ErrorDTO{"io_error", "owner file unreadable", {}});
        },
        EmptyLivenessSet());
    auto ready = Ready();
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
    auto cycle_intent = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                               owner.GetAuthoringAdmission().admission_generation);
    cycle_intent.owner_transaction_id = ProgramRunNumber(1U);
    const auto cycle = owner.AcquireCellAdmissionClaim(cycle_intent);
    ASSERT_TRUE(cycle.ok());
    // Exact identity and kind refusals are unchanged.
    auto wrong_generation = Release(cycle.value());
    wrong_generation.claim_generation =
        CellAdmissionClaimGeneration(cycle.value().claim_generation.value() + 100U);
    EXPECT_EQ(owner.ReleaseCellAdmissionClaim(wrong_generation).error().details.at("reason"),
              "claim_release_not_exact");
    auto wrong_kind = Release(cycle.value());
    wrong_kind.settlement.settlement_kind = CellAdmissionSettlementKindDTO::kOwnerCommit;
    EXPECT_EQ(owner.ReleaseCellAdmissionClaim(wrong_kind).error().details.at("reason"),
              "claim_release_not_exact");
    ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(cycle.value())).ok());

    ready.program_cycle_status->state = ProgramCycleStateDTO::kResetting;
    ready.program_cycle_status->accepted_reset = AcceptedReset();
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
    const auto identity = ProgramResetClaimRequestKey(*ready.program_cycle_status);
    ASSERT_TRUE(identity.ok());
    auto reset_intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
        owner.GetAuthoringAdmission().admission_generation, "reset.claim");
    reset_intent.request_key = identity.value();
    const auto reset = owner.AcquireProgramResetClaim(reset_intent, *ready.program_cycle_status);
    ASSERT_TRUE(reset.ok());
    auto reset_release = Release(reset.value());
    reset_release.request_key = AcceptedReset().request_key;
    auto reset_wrong_kind = reset_release;
    reset_wrong_kind.settlement.settlement_kind =
        CellAdmissionSettlementKindDTO::kTerminalEffectsSettled;
    EXPECT_EQ(owner.ReleaseCellAdmissionClaim(reset_wrong_kind).error().details.at("reason"),
              "claim_release_not_exact");
    ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(reset_release).ok());

    EXPECT_EQ(validator_calls, 0U);
    if (unreadable) {
      EXPECT_EQ(std::filesystem::file_size(journal_path), 0U);
      EXPECT_EQ(std::filesystem::status(journal_path).permissions(),
                std::filesystem::perms::none);
      std::filesystem::permissions(journal_path, std::filesystem::perms::owner_all);
    } else {
      EXPECT_FALSE(std::filesystem::exists(journal_path));
    }
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root),
                            std::filesystem::directory_iterator{}),
              unreadable ? 1 : 0);
    std::filesystem::remove_all(root);
  }
}

TEST(KernelCellAdmissionOwner, UnconfiguredManagerLivenessNeverOpensAuthoring) {
  FixedClock clock;
  AdmissionJournal journal;
  KernelCellAdmissionOwner owner(16U, clock, journal.Poster(), {});
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
      owner.Publish(LivenessObservation(set, 0U, 102U, ManagerLivenessStateDTO::kUnreconciled, true))
          .ok());
  // The generation advances in memory only: no journal record is posted.
  const auto expired_generation = owner.GetAuthoringAdmission().admission_generation;
  const auto expired_journal_sequence = journal.sequence.load();
  EXPECT_EQ(expired_generation.value(), current_generation.value() + 1U);
  EXPECT_EQ(expired_journal_sequence, current_journal_sequence);
  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 103U, ManagerLivenessStateDTO::kUnreconciled, true))
          .ok());
  EXPECT_EQ(owner.GetAuthoringAdmission().admission_generation, expired_generation);
  EXPECT_EQ(journal.sequence.load(), expired_journal_sequence);

  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 104U, ManagerLivenessStateDTO::kCurrent)).ok());
  EXPECT_EQ(owner.GetAuthoringAdmission().admission_generation.value(),
            expired_generation.value() + 1U);
  EXPECT_EQ(journal.sequence.load(), expired_journal_sequence);
}

TEST(KernelCellAdmissionOwner, ManagerLivenessReplacementClearsRows) {
  FixedClock clock;
  AdmissionJournal journal;
  const auto set = LivenessSet();
  auto owner = LivenessOwner(clock, journal, set);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());

  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 0U, 1U, ManagerLivenessStateDTO::kCurrent)).ok());
  ASSERT_TRUE(
      owner.Publish(LivenessObservation(set, 1U, 1U, ManagerLivenessStateDTO::kCurrent)).ok());
  ASSERT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);
  auto replacement = LivenessSet(set.required_receivers, RuntimeUnitId("manager.2"));
  const auto before = owner.GetAuthoringAdmission().admission_generation;
  const auto replaced = owner.ReplaceManagerLivenessRequiredSet(replacement);
  ASSERT_TRUE(replaced.ok());
  EXPECT_FALSE(replaced.value().authoring_allowed);
  EXPECT_GT(replaced.value().admission_generation, before);
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
      CellAdmissionClaimKindDTO::kManagerApply,  CellAdmissionClaimKindDTO::kAuthoredScene,
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
    conflicting.scope.resources = {ResourceId("conflict.resource")};
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

TEST(KernelCellAdmissionOwner, HeldManualEvidenceCannotImpersonateAnotherOwnerOrParent) {
  auto intent = Intent(CellAdmissionClaimKindDTO::kManualOperation, AdmissionGeneration(1U));
  intent.scope.resources = {ResourceId("arm.joint")};
  intent.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
      intent.owner_role, intent.owner_transaction_id, ContentDigest(kDigest),
      CellAdmissionClaimGeneration(1U)};
  ASSERT_TRUE(ValidateCellAdmissionClaimIntent(intent).ok());
  for (int change = 0; change < 5; ++change) {
    auto bad = intent;
    auto& evidence = *bad.intervention_evidence;
    if (change == 0) evidence.offering_owner_role = ApplicationRole::kProgramRunner;
    if (change == 1) evidence.offer_transaction_id = OperationNumber(999U);
    if (change == 2) evidence.parent_program_cycle_claim_generation.reset();
    if (change == 3) bad.scope.resources.clear();
    if (change == 4) {
      bad.claim_kind = CellAdmissionClaimKindDTO::kAuthoringMutation;
      bad.owner_role = ApplicationRole::kAuthoringOwner;
    }
    EXPECT_FALSE(ValidateCellAdmissionClaimIntent(bad).ok()) << change;
  }
}

TEST(KernelCellAdmissionOwner, HeldManualClaimNeedsCurrentOfferAndRetainsProgramHold) {
  FixedClock clock;
  AdmissionJournal journal;
  bool offer_current = false;
  unsigned checks = 0U;
  KernelCellAdmissionOwner owner{16U,
      clock,
      journal.Poster(),
      [&](const auto& intent, const auto& parent) {
        ++checks;
        EXPECT_TRUE(parent.has_value());
        if (!parent) return Result<void>::Err(ErrorCode::kInvalidStateTransition);
        EXPECT_EQ(parent_program_cycle_claim(intent), parent->claim_generation);
        return offer_current ? Result<void>::Ok()
                             : Result<void>::Err(ErrorCode::kInvalidStateTransition);
      },
      [](const auto&, const auto&) { return Result<void>::Ok(); },
      EmptyLivenessSet()};
  auto ready = Ready();
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  EXPECT_TRUE(owner.CheckManualCellAdmissionReadiness(std::nullopt).ok());
  auto program = Intent(CellAdmissionClaimKindDTO::kProgramCycle,
                        owner.GetAuthoringAdmission().admission_generation);
  program.owner_transaction_id = ProgramRunNumber(1U);
  const auto parent = owner.AcquireCellAdmissionClaim(program);
  ASSERT_TRUE(parent.ok());
  EXPECT_FALSE(owner.CheckManualCellAdmissionReadiness(std::nullopt).ok());
  EXPECT_FALSE(owner.CheckManualCellAdmissionReadiness(parent.value().claim_generation).ok());
  auto manual = Intent(CellAdmissionClaimKindDTO::kManualOperation,
                       owner.GetAuthoringAdmission().admission_generation, "manual.1");
  manual.scope.resources = {ResourceId("arm.joint")};
  manual.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
      manual.owner_role, manual.owner_transaction_id, ContentDigest(kDigest),
      parent.value().claim_generation};
  // A current Idle predecessor is insufficient for this handover.
  EXPECT_FALSE(owner.AcquireCellAdmissionClaim(manual).ok());
  EXPECT_EQ(checks, 0U);
  ready.program_cycle_status =
      ProgramCycleStatusDTO{2U,
                            ProgramCycleStateDTO::kRunning,     ProgramRunNumber(1U),
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

  manual.request_key = TestRequestKey("manual.2");
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

TEST(KernelCellAdmissionOwner, CapacityAndForeignOrStaleReleaseFailClosed) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal, 1U);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  auto claim =
      owner.AcquireCellAdmissionClaim(Intent(CellAdmissionClaimKindDTO::kAuthoringMutation,
                                             owner.GetAuthoringAdmission().admission_generation));
  ASSERT_TRUE(claim.ok());
  // An old generation alone is no refusal; the full ledger is.
  auto stale_intent = Intent(CellAdmissionClaimKindDTO::kAuthoringMutation, AdmissionGeneration(1U),
                             "request.stale");
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(stale_intent).error().code, "request_ledger_full");
  EXPECT_EQ(owner
                .AcquireCellAdmissionClaim(Intent(
                    CellAdmissionClaimKindDTO::kAuthoringMutation,
                    owner.GetAuthoringAdmission().admission_generation, "request.while-held"))
                .error()
                .code,
            "request_ledger_full");
  auto foreign = Release(claim.value());
  foreign.owner_role = ApplicationRole::kManager;
  EXPECT_EQ(owner.ReleaseCellAdmissionClaim(foreign).error().code, "stale_generation");
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(Release(claim.value())).ok());
  EXPECT_EQ(owner.AcquireCellAdmissionClaim(claim.value().intent).error().code,
            "request_result_expired");
  auto conflicting = claim.value().intent;
  conflicting.scope.resources = {ResourceId("conflict.resource")};
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
  EXPECT_EQ(journal.sequence.load(), after_acquire);

  auto conflicting_intent = claim.value().intent;
  conflicting_intent.scope.resources = {ResourceId("conflict.resource")};
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

// Contract 1.15.2: a release carries the key of the request that causes it.
// The release names its claim by role, transaction id and both generations;
// its own key is recorded but never compared with the claim's.
void ExpectCausingKeyReleaseSettles(KernelCellAdmissionOwner& owner,
                                    const CellAdmissionClaimDTO& claim) {
  const auto kind = to_string(claim.intent.claim_kind);
  auto release = Release(claim);
  release.request_key = TestRequestKey("causing." + std::string(kind));
  ASSERT_NE(release.request_key, claim.intent.request_key) << kind;
  auto wrong_generation = release;
  wrong_generation.claim_generation =
      CellAdmissionClaimGeneration(claim.claim_generation.value() + 100U);
  EXPECT_EQ(owner.ReleaseCellAdmissionClaim(wrong_generation).error().code, "stale_generation")
      << kind;
  auto wrong_acquired = release;
  wrong_acquired.acquired_admission_generation =
      AdmissionGeneration(claim.acquired_admission_generation.value() + 100U);
  EXPECT_EQ(owner.ReleaseCellAdmissionClaim(wrong_acquired).error().code, "stale_generation")
      << kind;
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(release).ok()) << kind;
  const auto receipt = owner.GetCellAdmissionRecoveryReceipt({claim.intent, claim, release});
  ASSERT_TRUE(receipt.ok()) << kind;
  EXPECT_EQ(receipt.value().status, CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact) << kind;
  EXPECT_EQ(receipt.value().matched_release, release) << kind;
  EXPECT_EQ(owner.ReleaseCellAdmissionClaim(release).error().code, "stale_generation") << kind;
}

TEST(KernelCellAdmissionOwner, ReleaseCarriesTheCausingRequestKeyForEveryClaimKind) {
  for (const auto kind :
       {CellAdmissionClaimKindDTO::kProgramCycle, CellAdmissionClaimKindDTO::kAuthoringMutation,
        CellAdmissionClaimKindDTO::kManagerApply, CellAdmissionClaimKindDTO::kAuthoredScene,
        CellAdmissionClaimKindDTO::kManualOperation}) {
    FixedClock clock;
    AdmissionJournal journal;
    auto owner = Owner(clock, journal);
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    const auto claim = owner.AcquireCellAdmissionClaim(
        Intent(kind, owner.GetAuthoringAdmission().admission_generation));
    ASSERT_TRUE(claim.ok()) << to_string(kind);
    ExpectCausingKeyReleaseSettles(owner, claim.value());
  }
  {
    FixedClock clock;
    AdmissionJournal journal;
    auto owner = Owner(clock, journal);
    const auto claim = owner.AcquireCellAdmissionClaim(
        Intent(CellAdmissionClaimKindDTO::kInterventionOrRecovery,
               owner.GetAuthoringAdmission().admission_generation));
    ASSERT_TRUE(claim.ok());
    ExpectCausingKeyReleaseSettles(owner, claim.value());
  }
  {
    FixedClock clock;
    AdmissionJournal journal;
    auto ready = Ready();
    auto owner = Owner(clock, journal);
    ready.program_cycle_status->state = ProgramCycleStateDTO::kResetting;
    ready.program_cycle_status->accepted_reset = AcceptedReset();
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
    const auto identity = ProgramResetClaimRequestKey(*ready.program_cycle_status);
    ASSERT_TRUE(identity.ok());
    auto intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
                         owner.GetAuthoringAdmission().admission_generation, "reset.claim");
    intent.request_key = identity.value();
    const auto claim = owner.AcquireProgramResetClaim(intent, *ready.program_cycle_status);
    ASSERT_TRUE(claim.ok());
    // The Reset claim is part 2 of the Reset's key; its release carries the
    // Reset's own key, part 0.
    auto release = Release(claim.value());
    release.request_key = AcceptedReset().request_key;
    ASSERT_NE(release.request_key, claim.value().intent.request_key);
    auto wrong_generation = release;
    wrong_generation.claim_generation =
        CellAdmissionClaimGeneration(claim.value().claim_generation.value() + 100U);
    EXPECT_EQ(owner.ReleaseCellAdmissionClaim(wrong_generation).error().code, "stale_generation");
    ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(release).ok());
  }
}

TEST(CellAdmission, RecoveryQueryAcceptsAReleaseWithTheCausingRequestKey) {
  CellAdmissionClaimDTO claim{
      Intent(CellAdmissionClaimKindDTO::kProgramCycle, AdmissionGeneration(3U)),
      CellAdmissionClaimGeneration(4U), AdmissionGeneration(3U), 1};
  auto release = Release(claim);
  release.request_key = TestRequestKey("causing.reset");
  EXPECT_TRUE(ValidateCellAdmissionRecoveryReceiptQuery({claim.intent, claim, release}).ok());
  release.claim_generation = CellAdmissionClaimGeneration(5U);
  EXPECT_EQ(ValidateCellAdmissionRecoveryReceiptQuery({claim.intent, claim, release})
                .error()
                .details.at("reason"),
            "recovery_expected_release_not_exact");
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

TEST(KernelCellAdmissionOwner, RecoveryFootprintIsAggregateEncodingSafe) {
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
        AdmissionGeneration(2U), {{claim, std::nullopt}});
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
      AdmissionGeneration(2U), {{boundary_minus_one, std::nullopt}});
  const auto rejected = ComputeCellAdmissionRecoveryFootprint(
      AdmissionGeneration(2U), {{over, std::nullopt}});
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
      AdmissionGeneration(3U),
      {{boundary_minus_one, std::nullopt}, {recovery_claim, std::nullopt}});
  ASSERT_TRUE(parent_and_recovery.ok());
  // Integer transaction ids and fixed-size request keys keep a parent and its
  // one recovery claim (the most an intervention may join) within every bound.
  EXPECT_TRUE(parent_and_recovery.value().admitted());

  // The aggregate bound still refuses a reachable state: a Program cycle
  // claim holding concurrent Program-node children. Each claim alone fits;
  // the held inventory of the third does not.
  FixedClock child_clock;
  AdmissionJournal child_journal;
  auto child_owner = Owner(child_clock, child_journal, 16U,
                           [](const auto&) { return Result<void>::Ok(); });
  auto child_ready = Ready();
  ASSERT_TRUE(child_owner.UpdateCellAdmissionReadiness(child_ready).ok());
  auto parent_intent = boundary_minus_one.intent;
  parent_intent.expected_admission_generation =
      child_owner.GetAuthoringAdmission().admission_generation;
  const auto cycle_parent = child_owner.AcquireCellAdmissionClaim(parent_intent);
  ASSERT_TRUE(cycle_parent.ok()) << cycle_parent.error().message;
  child_ready.program_cycle_status =
      ProgramCycleStatusDTO{2U, ProgramCycleStateDTO::kRunning, ProgramRunNumber(105U),
                            cycle_parent.value().claim_generation, std::nullopt};
  ASSERT_TRUE(child_owner.UpdateCellAdmissionReadiness(child_ready).ok());
  const auto program_child = [&](const std::string& node) {
    auto child = Intent(CellAdmissionClaimKindDTO::kAuthoredScene,
                        child_owner.GetAuthoringAdmission().admission_generation,
                        "footprint.child." + node);
    child.scope.resources = boundary_minus_one.intent.scope.resources;
    child.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{
        ProgramRunNumber(105U), cycle_parent.value().claim_generation, node,
        NodeAttemptNumber(1U), "scene.carried_attachment"};
    return child;
  };
  const auto first_child = child_owner.AcquireCellAdmissionClaim(program_child("attach.1"));
  ASSERT_TRUE(first_child.ok()) << first_child.error().message;
  const auto child_sequence_before = child_journal.sequence.load();
  const auto second_child = child_owner.AcquireCellAdmissionClaim(program_child("attach.2"));
  ASSERT_FALSE(second_child.ok());
  EXPECT_EQ(second_child.error().code, "invalid_request");
  EXPECT_EQ(second_child.error().details.at("reason"), "recovery_footprint_exceeded");
  EXPECT_EQ(child_journal.sequence.load(), child_sequence_before);

  const auto ordinary =
      make_claim(1U, 'x', CellAdmissionClaimGeneration(7U), AdmissionGeneration(7U));
  const auto released_form = ComputeCellAdmissionRecoveryFootprint(
      AdmissionGeneration(8U),
      {{ordinary, Release(ordinary)}});
  ASSERT_TRUE(released_form.ok());
  EXPECT_TRUE(released_form.value().admitted());
}

// Per claim kind: acquire with the typed transaction number (or none), exact
// replay, conflicting replay, release by identity, and expiry after release.
TEST(KernelCellAdmissionOwner, TypedTransactionIdentityReplayConflictReleaseAndExpiry) {
  for (const auto kind :
       {CellAdmissionClaimKindDTO::kProgramCycle, CellAdmissionClaimKindDTO::kAuthoringMutation,
        CellAdmissionClaimKindDTO::kManagerApply, CellAdmissionClaimKindDTO::kAuthoredScene,
        CellAdmissionClaimKindDTO::kManualOperation}) {
    FixedClock clock;
    AdmissionJournal journal;
    auto owner = Owner(clock, journal);
    ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
    const auto intent = Intent(kind, owner.GetAuthoringAdmission().admission_generation);
    ASSERT_EQ(intent.owner_transaction_id, TransactionId(kind)) << to_string(kind);
    const auto acquired = owner.AcquireCellAdmissionClaim(intent);
    ASSERT_TRUE(acquired.ok()) << to_string(kind) << ": " << acquired.error().message;
    EXPECT_EQ(acquired.value().intent, intent) << to_string(kind);
    const auto sequence = journal.sequence.load();
    EXPECT_EQ(owner.AcquireCellAdmissionClaim(intent).value(), acquired.value()) << to_string(kind);
    EXPECT_EQ(journal.sequence.load(), sequence) << to_string(kind);

    auto other_scope = intent;
    other_scope.scope.resources = {ResourceId("conflict.resource")};
    EXPECT_EQ(owner.AcquireCellAdmissionClaim(other_scope).error().code, "request_id_conflict")
        << to_string(kind);
    if (intent.owner_transaction_id) {
      auto other_number = intent;
      other_number.owner_transaction_id = TransactionId(kind, 106U);
      EXPECT_EQ(owner.AcquireCellAdmissionClaim(other_number).error().code,
                "request_id_conflict")
          << to_string(kind);
      // A release naming another transaction of the same role is not this claim's.
      auto foreign_release = Release(acquired.value());
      foreign_release.owner_transaction_id = TransactionId(kind, 106U);
      EXPECT_EQ(owner.ReleaseCellAdmissionClaim(foreign_release).error().code,
                "stale_generation")
          << to_string(kind);
    }

    const auto release = Release(acquired.value());
    EXPECT_EQ(release.owner_transaction_id, intent.owner_transaction_id);
    ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(release).ok()) << to_string(kind);
    EXPECT_EQ(owner.AcquireCellAdmissionClaim(intent).error().code, "request_result_expired")
        << to_string(kind);
    EXPECT_EQ(owner.AcquireCellAdmissionClaim(other_scope).error().code, "request_id_conflict")
        << to_string(kind);
  }
}

// The Program Reset claim carries no transaction number and is replayed,
// conflicted, released and expired by its request key alone.
TEST(KernelCellAdmissionOwner, ProgramResetClaimWithoutTransactionIdReplaysAndExpires) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  auto ready = Ready();
  ready.program_cycle_status->state = ProgramCycleStateDTO::kResetting;
  ready.program_cycle_status->accepted_reset = AcceptedReset();
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(ready).ok());
  const auto identity = ProgramResetClaimRequestKey(*ready.program_cycle_status);
  ASSERT_TRUE(identity.ok());
  auto intent = Intent(CellAdmissionClaimKindDTO::kProgramReset,
                       owner.GetAuthoringAdmission().admission_generation, "reset.claim");
  intent.request_key = identity.value();
  ASSERT_FALSE(intent.owner_transaction_id.has_value());
  const auto carried = *ready.program_cycle_status;
  const auto acquired = owner.AcquireProgramResetClaim(intent, carried);
  ASSERT_TRUE(acquired.ok()) << acquired.error().message;
  EXPECT_EQ(owner.AcquireProgramResetClaim(intent, carried).value(), acquired.value());
  auto conflicting = intent;
  conflicting.expected_admission_generation =
      AdmissionGeneration(intent.expected_admission_generation.value() + 1U);
  EXPECT_EQ(owner.AcquireProgramResetClaim(conflicting, carried).error().code,
            "request_id_conflict");
  auto release = Release(acquired.value());
  release.request_key = AcceptedReset().request_key;
  EXPECT_FALSE(release.owner_transaction_id.has_value());
  ASSERT_TRUE(owner.ReleaseCellAdmissionClaim(release).ok());
  EXPECT_EQ(owner.AcquireProgramResetClaim(intent, carried).error().code,
            "request_result_expired");
}

// An intent's transaction number must be present exactly for a Program cycle,
// a Manager Apply and a manual operation, typed by its owner role and nonzero.
TEST(KernelCellAdmissionOwner, TransactionIdMustMatchTheClaimKind) {
  FixedClock clock;
  AdmissionJournal journal;
  auto owner = Owner(clock, journal);
  ASSERT_TRUE(owner.UpdateCellAdmissionReadiness(Ready()).ok());
  const auto generation = owner.GetAuthoringAdmission().admission_generation;
  const auto before = journal.sequence.load();
  const auto refused = [&](CellAdmissionClaimKindDTO kind,
                           std::optional<CellAdmissionOwnerTransactionId> id) {
    auto intent = Intent(kind, generation, "typed.refused");
    intent.owner_transaction_id = std::move(id);
    const auto result = owner.AcquireCellAdmissionClaim(intent);
    ASSERT_FALSE(result.ok()) << to_string(kind);
    EXPECT_EQ(result.error().code, "invalid_request") << to_string(kind);
    EXPECT_EQ(result.error().details.at("reason"), "claim_identity_invalid") << to_string(kind);
  };
  // Missing.
  refused(CellAdmissionClaimKindDTO::kProgramCycle, std::nullopt);
  refused(CellAdmissionClaimKindDTO::kManagerApply, std::nullopt);
  refused(CellAdmissionClaimKindDTO::kManualOperation, std::nullopt);
  // Mistyped for the owner role.
  refused(CellAdmissionClaimKindDTO::kProgramCycle, ApplyId(105U));
  refused(CellAdmissionClaimKindDTO::kManagerApply, OperationNumber(105U));
  refused(CellAdmissionClaimKindDTO::kManualOperation, ProgramRunNumber(105U));
  // Zero.
  refused(CellAdmissionClaimKindDTO::kProgramCycle, ProgramRunNumber(0U));
  refused(CellAdmissionClaimKindDTO::kManagerApply, ApplyId(0U));
  // Present for a kind that carries none.
  refused(CellAdmissionClaimKindDTO::kProgramReset, ProgramRunNumber(1U));
  refused(CellAdmissionClaimKindDTO::kAuthoringMutation, ProgramRunNumber(1U));
  refused(CellAdmissionClaimKindDTO::kAuthoredScene, ApplyId(1U));
  refused(CellAdmissionClaimKindDTO::kInterventionOrRecovery, OperationNumber(1U));
  EXPECT_EQ(journal.sequence.load(), before);
  EXPECT_FALSE(owner.has_unresolved_claims());

  // A release id, when present, has its role's type.
  const auto claim = owner.AcquireCellAdmissionClaim(
      Intent(CellAdmissionClaimKindDTO::kManagerApply, generation, "typed.apply"));
  ASSERT_TRUE(claim.ok());
  auto mistyped = Release(claim.value());
  mistyped.owner_transaction_id = ProgramRunNumber(105U);
  const auto release = owner.ReleaseCellAdmissionClaim(mistyped);
  ASSERT_FALSE(release.ok());
  EXPECT_EQ(release.error().code, "invalid_request");
  EXPECT_EQ(release.error().details.at("reason"), "release_identity_invalid");
  EXPECT_TRUE(owner.ReleaseCellAdmissionClaim(Release(claim.value())).ok());
}

}  // namespace
