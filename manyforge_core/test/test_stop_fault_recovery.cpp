#include <gtest/gtest.h>

#include <manyforge_core/stop_fault_recovery.hpp>

namespace manyforge::core {
namespace {

inline constexpr char kDigestA[] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
inline constexpr char kDigestB[] =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
inline constexpr char kDigestC[] =
    "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
inline constexpr char kDigestD[] =
    "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
inline constexpr char kDigestE[] =
    "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";

[[nodiscard]] StopAcceptedDTO Accepted() {
  return StopAcceptedDTO{RequestId("stop-request.1"),
                         RequestReplayStatusDTO::kAcceptedNew,
                         StopId("stop.1"),
                         StopGeneration(1U),
                         ContentDigest(kDigestA),
                         StopResolutionDispositionDTO::kExact,
                         StopScopeResolutionReasonDTO::kExplicitFullStop,
                         {},
                         ContentDigest(kDigestB),
                         {},
                         {},
                         1'000,
                         100,
                         900,
                         10,
                         {}};
}

[[nodiscard]] StopCertificateDTO Certificate() {
  auto reduced = ReduceStopTerminal(StopTerminalReductionInputDTO{
      StopCertificateId("certificate.1"),
      StopCertificateRevision(1U),
      StopId("stop.1"),
      StopGeneration(1U),
      StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kExplicitFull,
                                       ContentDigest(kDigestA), std::nullopt, std::nullopt},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExplicitFullStop,
      {},
      {StopScopeCaptureHistoryDTO{1U,
                                  StopResolutionDispositionDTO::kExact,
                                  StopScopeResolutionReasonDTO::kExplicitFullStop,
                                  ContentDigest(kDigestB),
                                  {},
                                  {},
                                  {}}},
      {},
      {},
      {},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      20,
      std::nullopt});
  EXPECT_TRUE(reduced.ok()) << reduced.error().message;
  return std::move(reduced.value());
}

[[nodiscard]] StopFaultLatchDTO Latch(const StopCertificateDTO& certificate) {
  return StopFaultLatchDTO{
      CellId("cell.1"),
      certificate.stop_id,
      certificate.stop_generation,
      StopCertificateRefDTO{certificate.stop_certificate_id, certificate.stop_certificate_revision,
                            certificate.stop_id, certificate.stop_generation},
      StopFaultLatchId("latch.1"),
      StopFaultLatchGeneration(1U),
      StopFaultTriggerDTO::kScopePromotion,
      false,
      {},
      {ResourceId("resource.1")},
      RetainedBundleId(kDigestA),
      GraphRevision(kDigestB),
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                     std::nullopt},
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                     std::nullopt},
      StopFaultLatchStateDTO::kLatched,
      1U,
      StopFaultRecoveryStatusDTO::kWaitingForObservation,
      "stop_fault_latched",
      {}};
}

[[nodiscard]] StopStatusDTO Status() {
  const auto certificate = Certificate();
  return StopStatusDTO{
      Accepted(),
      StopStatusStateDTO::kSucceeded,
      certificate.scope_entries,
      {},
      certificate.attempted_stage_results,
      certificate.evidence_items,
      certificate.unsettled,
      certificate.failed,
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                     std::nullopt},
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                     std::nullopt},
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable, std::nullopt,
                                     std::nullopt},
      StopCertificateRefDTO{certificate.stop_certificate_id, certificate.stop_certificate_revision,
                            certificate.stop_id, certificate.stop_generation},
      Latch(certificate),
      std::nullopt};
}

[[nodiscard]] StopFaultRecoveryConfigurationExpectationDTO Configuration() {
  return StopFaultRecoveryConfigurationExpectationDTO{
      ControlDomainId("domain.1"), ActiveControlConfigurationId("configuration.1"),
      ActiveControlConfigurationGeneration(1U)};
}

[[nodiscard]] StopFaultRecoveryFactsDTO Facts(const StopStatusDTO& status) {
  return StopFaultRecoveryFactsDTO{
      100,
      status.fault_latch->cell_id,
      status.fault_latch->retained_bundle_id,
      status.fault_latch->graph_revision,
      *status.stop_certificate,
      status.fault_latch->latch_id,
      status.fault_latch->latch_generation,
      status.acceptance_projection,
      status.terminal_projection,
      status.surveillance_projection,
      status.surveillance,
      {StopFaultRecoveryResourceFactDTO{
          ResourceId("resource.1"), StopFaultRecoveryResourceStateDTO::kSettled,
          RuntimeEpoch("state.epoch"), 1U, 1'000, 10'000, ContentDigest(kDigestC), std::nullopt}},
      {StopFaultRecoveryConfigurationFactDTO{Configuration(), true, ContentDigest(kDigestD)}},
      {},
      {},
      {},
      {},
      {}};
}

[[nodiscard]] StopFaultRecoveryIntentDTO Intent(const StopFaultLatchDTO& latch) {
  return StopFaultRecoveryIntentDTO{
      PublicRequestContextDTO{RequestId("recovery.1"), RequestOwnerEpoch("owner.1"), "test",
                              std::nullopt},
      latch.latch_id,
      latch.latch_generation,
      latch.recovery_assessment_revision,
      latch.stop_certificate->stop_certificate_id,
      latch.stop_certificate->stop_certificate_revision,
      latch.cell_id,
      latch.retained_bundle_id,
      latch.graph_revision,
      {Configuration()},
      std::string("operator verified cell")};
}

[[nodiscard]] StopStatusDTO ReadyStatus() {
  auto status = Status();
  auto assessment = AssessStopFaultRecovery(status, Facts(status));
  EXPECT_TRUE(assessment.ok()) << assessment.error().message;
  status.fault_latch = std::move(assessment.value());
  return status;
}

TEST(StopFaultRecoveryTest, AssessmentUsesFailClosedPredicatePriority) {
  auto status = Status();

  auto durable_facts = Facts(status);
  durable_facts.terminal_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kPending, std::nullopt, std::nullopt};
  status.terminal_projection = durable_facts.terminal_projection;
  status.fault_latch->terminal_projection = durable_facts.terminal_projection;
  auto assessed = AssessStopFaultRecovery(status, durable_facts);
  ASSERT_TRUE(assessed.ok()) << assessed.error().message;
  EXPECT_EQ(assessed.value().recovery_status, StopFaultRecoveryStatusDTO::kWaitingForDurableRecord);

  status = Status();
  auto unavailable_facts = Facts(status);
  unavailable_facts.resource_facts.front() =
      StopFaultRecoveryResourceFactDTO{ResourceId("resource.1"),
                                       StopFaultRecoveryResourceStateDTO::kUnavailable,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       ErrorDTO{"observed_state_unavailable", "unavailable", {}}};
  assessed = AssessStopFaultRecovery(status, unavailable_facts);
  ASSERT_TRUE(assessed.ok()) << assessed.error().message;
  EXPECT_EQ(assessed.value().recovery_status, StopFaultRecoveryStatusDTO::kWaitingForObservation);

  auto unsettled_facts = Facts(status);
  unsettled_facts.resource_facts.front().state = StopFaultRecoveryResourceStateDTO::kUnsettled;
  assessed = AssessStopFaultRecovery(status, unsettled_facts);
  ASSERT_TRUE(assessed.ok()) << assessed.error().message;
  EXPECT_EQ(assessed.value().recovery_status, StopFaultRecoveryStatusDTO::kWaitingForQuiescence);

  auto configuration_facts = Facts(status);
  configuration_facts.configuration_facts.front().reconciled = false;
  configuration_facts.configuration_facts.front().evidence_digest.reset();
  assessed = AssessStopFaultRecovery(status, configuration_facts);
  ASSERT_TRUE(assessed.ok()) << assessed.error().message;
  EXPECT_EQ(assessed.value().recovery_status, StopFaultRecoveryStatusDTO::kWaitingForConfiguration);
}

// Q32: a component-incident containment is released once its Stop finished
// as the controller/backend reported; an unsettled or unavailable measured-rest
// observation no longer holds it.
TEST(StopFaultRecoveryTest, IncidentContainmentIsNotGatedOnMeasuredRest) {
  auto status = Status();
  status.fault_latch->trigger = StopFaultTriggerDTO::kPhysicalRestUnproven;
  status.fault_latch->reason_code = "stop_physical_rest_unproven";
  auto unsettled_facts = Facts(status);
  unsettled_facts.resource_facts.front().state = StopFaultRecoveryResourceStateDTO::kUnsettled;
  auto assessed = AssessStopFaultRecovery(status, unsettled_facts);
  ASSERT_TRUE(assessed.ok()) << assessed.error().message;
  EXPECT_EQ(assessed.value().recovery_status, StopFaultRecoveryStatusDTO::kReady);
}

TEST(StopFaultRecoveryTest, ReadyAssessmentIsStableUntilSemanticFactsChange) {
  auto status = Status();
  const auto facts = Facts(status);
  auto ready = AssessStopFaultRecovery(status, facts);
  ASSERT_TRUE(ready.ok()) << ready.error().message;
  EXPECT_EQ(ready.value().recovery_status, StopFaultRecoveryStatusDTO::kReady);
  EXPECT_EQ(ready.value().recovery_assessment_revision, 2U);
  ASSERT_EQ(ready.value().evidence_refs.size(), 2U);

  status.fault_latch = ready.value();
  auto replay = AssessStopFaultRecovery(status, facts);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value(), ready.value());

  auto refreshed = facts;
  ++*refreshed.resource_facts.front().source_sequence;
  refreshed.resource_facts.front().source_age_ns = 0;
  refreshed.resource_facts.front().evidence_digest = ContentDigest(kDigestE);
  replay = AssessStopFaultRecovery(status, refreshed);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value(), ready.value());

  auto blocked = facts;
  blocked.fenced_or_orphan_resources.push_back(ResourceId("resource.2"));
  auto changed = AssessStopFaultRecovery(status, blocked);
  ASSERT_TRUE(changed.ok()) << changed.error().message;
  EXPECT_EQ(changed.value().recovery_status, StopFaultRecoveryStatusDTO::kWaitingForObservation);
  EXPECT_EQ(changed.value().recovery_assessment_revision, 3U);
}

TEST(StopFaultRecoveryTest, TerminalFaultMustCarryItsExactCertificateBeforeAssessment) {
  auto status = Status();
  const auto facts = Facts(status);
  status.fault_latch->stop_certificate.reset();
  EXPECT_TRUE(stop_status_field_error(status).has_value());
  EXPECT_FALSE(AssessStopFaultRecovery(status, facts).ok());
  status.fault_latch->stop_certificate = facts.stop_certificate;
  status.fault_latch->stop_certificate->stop_certificate_id =
      StopCertificateId("wrong-certificate");
  EXPECT_TRUE(stop_status_field_error(status).has_value());
  EXPECT_FALSE(AssessStopFaultRecovery(status, facts).ok());
}

TEST(StopFaultRecoveryTest, EligibleEvaluationReturnsUnpublishedClearedStatusCandidate) {
  const auto status = ReadyStatus();
  const auto intent = Intent(*status.fault_latch);
  auto evaluation = EvaluateStopFaultRecovery(intent, status, Facts(status));
  ASSERT_TRUE(evaluation.ok()) << evaluation.error().message;
  EXPECT_TRUE(evaluation.value().result.accepted);
  EXPECT_FALSE(evaluation.value().result.error.has_value());
  ASSERT_TRUE(evaluation.value().resulting_stop_status.has_value());
  EXPECT_EQ(evaluation.value().result.latch.state, StopFaultLatchStateDTO::kCleared);
  EXPECT_EQ(evaluation.value().result.latch.clear_projection.state,
            StopDurableProjectionStateDTO::kRecorded);
  EXPECT_EQ(evaluation.value().resulting_stop_status->fault_latch, evaluation.value().result.latch);
  EXPECT_FALSE(stop_fault_recovery_evaluation_field_error(evaluation.value()).has_value());
}

TEST(StopFaultRecoveryTest, StaleIntentReturnsNewestLatchWithoutClearCandidate) {
  const auto status = ReadyStatus();
  auto intent = Intent(*status.fault_latch);
  intent.expected_recovery_assessment_revision -= 1U;
  auto evaluation = EvaluateStopFaultRecovery(intent, status, Facts(status));
  ASSERT_TRUE(evaluation.ok()) << evaluation.error().message;
  EXPECT_FALSE(evaluation.value().result.accepted);
  ASSERT_TRUE(evaluation.value().result.error.has_value());
  EXPECT_EQ(evaluation.value().result.error->code, "stop_fault_recovery_identity_mismatch");
  EXPECT_EQ(evaluation.value().result.latch, *status.fault_latch);
  EXPECT_FALSE(evaluation.value().resulting_stop_status.has_value());
}

TEST(StopFaultRecoveryTest, FactsRequireExactCapturedResourceCoverageAndCanonicalRows) {
  const auto status = Status();
  auto facts = Facts(status);
  facts.resource_facts.front().resource_id = ResourceId("other.resource");
  auto assessed = AssessStopFaultRecovery(status, facts);
  ASSERT_FALSE(assessed.ok());
  EXPECT_EQ(assessed.error().code, "stop_fault_recovery_facts_join_invalid");

  facts = Facts(status);
  facts.configuration_facts.push_back(facts.configuration_facts.front());
  EXPECT_EQ(stop_fault_recovery_facts_field_error(facts), "stop_fault_recovery_facts_invalid");
}

TEST(StopFaultRecoveryTest, LatchWithoutCapturedResourcesIsAssessableAndClearable) {
  // Live incident 2026-10-02: a Stop that captured only a never-dispatched
  // planning attempt latches no resource (it has no motion-rest obligation).
  // Its exact recovery facts are therefore resource-free and must still be
  // assessed and cleared, never rejected as incomplete.
  auto status = Status();
  status.fault_latch->trigger = StopFaultTriggerDTO::kPersistenceDegraded;
  status.fault_latch->persistence_degraded = true;
  status.fault_latch->captured_resources.clear();
  auto facts = Facts(status);
  facts.resource_facts.clear();
  EXPECT_FALSE(stop_fault_recovery_facts_field_error(facts).has_value());

  auto assessed = AssessStopFaultRecovery(status, facts);
  ASSERT_TRUE(assessed.ok()) << assessed.error().message;
  EXPECT_EQ(assessed.value().recovery_status, StopFaultRecoveryStatusDTO::kReady);
  status.fault_latch = assessed.value();
  auto evaluation = EvaluateStopFaultRecovery(Intent(*status.fault_latch), status, facts);
  ASSERT_TRUE(evaluation.ok()) << evaluation.error().message;
  EXPECT_TRUE(evaluation.value().result.accepted);
  EXPECT_EQ(evaluation.value().result.latch.state, StopFaultLatchStateDTO::kCleared);

  // Coverage stays exact: a resource fact the latch did not capture, or a
  // still-missing durable record, keeps the latch closed.
  auto widened = Facts(status);
  auto rejected = AssessStopFaultRecovery(status, widened);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "stop_fault_recovery_facts_join_invalid");
  status = Status();
  status.fault_latch->captured_resources.clear();
  status.acceptance_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kFailed, ErrorDTO{"journal_failed", "failed", {}}, 1U};
  facts = Facts(status);
  facts.resource_facts.clear();
  assessed = AssessStopFaultRecovery(status, facts);
  ASSERT_TRUE(assessed.ok()) << assessed.error().message;
  EXPECT_EQ(assessed.value().recovery_status, StopFaultRecoveryStatusDTO::kWaitingForDurableRecord);
}

}  // namespace
}  // namespace manyforge::core
