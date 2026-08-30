#include <gtest/gtest.h>

#include <limits>
#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/execution_recovery_support.hpp>
#include <manyforge_core/types/reason_codes.hpp>

namespace manyforge::core {
namespace {

KernelStopCertificateDTO make_certificate(StopOwnerKind owner, std::uint64_t generation,
                                          std::uint64_t execution_id, const std::string& latch_id) {
  KernelStopCertificateDTO certificate;
  certificate.certificate_id = "cert-g" + std::to_string(generation);
  certificate.completion_result = StopCertificateOutcome::kSucceeded;
  certificate.stop_owner = owner;
  certificate.stop_owner_generation = generation;
  certificate.execution_id = execution_id;
  certificate.latch_id = latch_id;
  certificate.backend_stop_confirmed = true;
  certificate.at_rest_evidence = AtRestEvidence::kUnavailable;
  return certificate;
}

InterruptLatch make_collision_latch(const std::string& latch_id, std::uint64_t execution_id,
                                    InterruptPreemptionResult result) {
  InterruptLatch latch;
  latch.latch_id = latch_id;
  latch.source_id = kRuntimeCollisionFaultSourceId;
  latch.policy_id = "runtime_collision_policy";
  latch.active_execution_id = execution_id;
  latch.preemption_result = result;
  return latch;
}

TEST(RecoveryCertificateStoreTest, LateOldGenerationCannotOverwriteNewer) {
  RecoveryCertificateStore store;
  store.publish(make_certificate(StopOwnerKind::kFaultPreemptWorker, 7, 42, "latch-7"));
  // Late completion from an OLDER owner generation arrives afterwards.
  store.publish(make_certificate(StopOwnerKind::kFaultPreemptWorker, 6, 41, "latch-6"));

  const auto authoritative = store.authoritative(StopOwnerKind::kFaultPreemptWorker);
  ASSERT_TRUE(authoritative.has_value());
  EXPECT_EQ(authoritative->stop_owner_generation, 7u);
  EXPECT_EQ(authoritative->latch_id, "latch-7");
  // The stale record remains retained evidence, not authority.
  EXPECT_EQ(store.evidence_ring().size(), 2u);
  // Exact-key admission lookup for the stale generation fails.
  EXPECT_FALSE(
      store.find_authoritative(StopOwnerKind::kFaultPreemptWorker, 41, "latch-6", 6).has_value());
}

TEST(RecoveryCertificateStoreTest, EvidenceRingIsBoundedAndDeterministic) {
  RecoveryCertificateStore store;
  for (std::uint64_t i = 1; i <= RecoveryCertificateStore::kEvidenceRingCapacity + 5; ++i) {
    store.publish(make_certificate(StopOwnerKind::kOperatorStopWorker, i, i, ""));
  }
  EXPECT_EQ(store.evidence_ring().size(), RecoveryCertificateStore::kEvidenceRingCapacity);
  // Oldest evicted first: the front is publication (5+1).
  EXPECT_EQ(store.evidence_ring().front().stop_owner_generation, 6u);
}

TEST(RecoveryCertificateStoreTest, EqualGenerationDifferentIdentityCannotReplaceAuthority) {
  RecoveryCertificateStore store;
  store.publish(make_certificate(StopOwnerKind::kFaultPreemptWorker, 7, 42, "latch-7"));
  store.publish(make_certificate(StopOwnerKind::kFaultPreemptWorker, 7, 99, "other"));
  const auto authoritative = store.authoritative(StopOwnerKind::kFaultPreemptWorker);
  ASSERT_TRUE(authoritative.has_value());
  EXPECT_EQ(authoritative->execution_id, 42u);
  EXPECT_EQ(authoritative->latch_id, "latch-7");
}

TEST(RecoveryCertificateStoreTest, OwnerSessionProgressUpgradesButNeverDuplicatesOrDowngrades) {
  RecoveryCertificateStore store;
  auto partial = make_certificate(StopOwnerKind::kOperatorStopWorker, 4, 17, "");
  partial.completion_result = StopCertificateOutcome::kPartial;
  partial.backend_stop_confirmed = false;
  ASSERT_TRUE(store.publish_owner_session_progress(partial).has_value());
  EXPECT_FALSE(store.publish_owner_session_progress(partial).has_value());

  auto succeeded = partial;
  succeeded.certificate_id = "cert-g4-succeeded";
  succeeded.completion_result = StopCertificateOutcome::kSucceeded;
  succeeded.backend_stop_confirmed = true;
  ASSERT_TRUE(store.publish_owner_session_progress(succeeded).has_value());
  EXPECT_FALSE(store.publish_owner_session_progress(succeeded).has_value());
  EXPECT_FALSE(store.publish_owner_session_progress(partial).has_value());

  const auto authoritative = store.authoritative(StopOwnerKind::kOperatorStopWorker);
  ASSERT_TRUE(authoritative.has_value());
  EXPECT_EQ(authoritative->completion_result, StopCertificateOutcome::kSucceeded);
  EXPECT_TRUE(authoritative->backend_stop_confirmed);
  EXPECT_EQ(store.evidence_ring().size(), 2u);
}

TEST(StopOwnerIdentityTrackerTest, BindsGenerationCoherentlyToExecution) {
  StopOwnerIdentityTracker tracker;
  const auto first = tracker.begin(17);
  EXPECT_NE(first, 0u);
  EXPECT_EQ(tracker.ensure(17), first);
  EXPECT_EQ(tracker.generation_for_execution(17), first);
  EXPECT_EQ(tracker.generation_for_execution(18), 0u);

  const auto second = tracker.ensure(18);
  EXPECT_GT(second, first);
  EXPECT_EQ(tracker.generation_for_execution(17), 0u);
  EXPECT_EQ(tracker.generation_for_execution(18), second);
}

TEST(ObservedReadinessTrackerTest, UnsafeRevokesImmediatelySafeNeedsDwell) {
  ObservedReadinessTracker tracker;
  RecoveryReadinessPolicy policy;
  policy.ready_dwell_ns = 100;
  policy.ready_min_samples = 2;

  ObservedStateEvidenceDTO sample;
  sample.complete = true;
  sample.at_rest = true;
  sample.sequence = 1;
  sample.freshness_budget_ns = 1'000;
  sample.joint_order_token = "tok";
  sample.source_identity = "controller_state";

  sample.sampled_at_ns = 0;
  EXPECT_FALSE(tracker.update(sample, "tok", policy, 0));  // 1 sample, no dwell
  sample.sampled_at_ns = 60;
  sample.sequence = 2;
  EXPECT_FALSE(tracker.update(sample, "tok", policy, 60));  // dwell 60 < 100
  sample.sampled_at_ns = 120;
  sample.sequence = 3;
  EXPECT_TRUE(tracker.update(sample, "tok", policy, 120));  // qualified

  // Single unsafe sample revokes instantly and resets the qualifier.
  ObservedStateEvidenceDTO moving = sample;
  moving.at_rest = false;
  moving.sampled_at_ns = 130;
  moving.sequence = 4;
  EXPECT_FALSE(tracker.update(moving, "tok", policy, 130));
  sample.sampled_at_ns = 140;
  sample.sequence = 5;
  EXPECT_FALSE(tracker.update(sample, "tok", policy, 140));  // dwell restarted
}

TEST(ObservedReadinessTrackerTest, FreshnessExpiryDisqualifiesWithoutSamples) {
  ObservedReadinessTracker tracker;
  RecoveryReadinessPolicy policy;
  policy.ready_dwell_ns = 0;
  policy.ready_min_samples = 1;

  ObservedStateEvidenceDTO sample;
  sample.complete = true;
  sample.at_rest = true;
  sample.sequence = 1;
  sample.freshness_budget_ns = 100;
  sample.joint_order_token = "tok";
  sample.source_identity = "controller_state";
  sample.sampled_at_ns = 0;
  EXPECT_TRUE(tracker.update(sample, "tok", policy, 0));
  // Time passes with no new sample: pure time re-evaluation disqualifies.
  EXPECT_FALSE(tracker.evaluate(policy, 100));
}

TEST(ObservedReadinessTrackerTest, WrongJointOrderTokenIsUnsafe) {
  ObservedReadinessTracker tracker;
  RecoveryReadinessPolicy policy;
  policy.ready_dwell_ns = 0;
  policy.ready_min_samples = 1;
  ObservedStateEvidenceDTO sample;
  sample.complete = true;
  sample.at_rest = true;
  sample.sequence = 1;
  sample.freshness_budget_ns = 100;
  sample.joint_order_token = "other";
  sample.source_identity = "controller_state";
  EXPECT_FALSE(tracker.update(sample, "expected", policy, 0));
}

TEST(ObservedReadinessTrackerTest, DuplicateSequenceDoesNotSatisfyConsecutiveSampleRequirement) {
  ObservedReadinessTracker tracker;
  RecoveryReadinessPolicy policy;
  policy.ready_min_samples = 2;
  ObservedStateEvidenceDTO sample;
  sample.sequence = 10;
  sample.complete = true;
  sample.at_rest = true;
  sample.freshness_budget_ns = 1000;
  sample.joint_order_token = "tok";
  sample.source_identity = "controller_state";
  EXPECT_FALSE(tracker.update(sample, "tok", policy, 0));
  EXPECT_FALSE(tracker.update(sample, "tok", policy, 10));
  sample.sequence = 11;
  sample.sampled_at_ns = 20;
  EXPECT_TRUE(tracker.update(sample, "tok", policy, 20));
}

TEST(ObservedReadinessTrackerTest, FutureTimestampFailsClosed) {
  ObservedReadinessTracker tracker;
  RecoveryReadinessPolicy policy;
  ObservedStateEvidenceDTO sample;
  sample.sequence = 1;
  sample.complete = true;
  sample.at_rest = true;
  sample.sampled_at_ns = 101;
  sample.freshness_budget_ns = 1000;
  sample.source_identity = "controller_state";
  EXPECT_FALSE(tracker.update(sample, "", policy, 100));
}

TEST(ObservedReadinessTrackerTest, MissingSourceIdentityFailsClosed) {
  ObservedReadinessTracker tracker;
  RecoveryReadinessPolicy policy;
  ObservedStateEvidenceDTO sample;
  sample.sequence = 1;
  sample.complete = true;
  sample.at_rest = true;
  sample.freshness_budget_ns = 1000;
  EXPECT_FALSE(tracker.update(sample, "", policy, 0));
}

TEST(ObservedReadinessTrackerTest,
     MeasuredPolicyRejectsPredictedAuthorityAndAcceptsMeasuredAuthority) {
  ObservedReadinessTracker tracker;
  RecoveryReadinessPolicy policy;
  policy.require_measured_observation = true;

  ObservedStateEvidenceDTO sample;
  sample.sequence = 1;
  sample.sampled_at_ns = 10;
  sample.freshness_budget_ns = 100;
  sample.complete = true;
  sample.at_rest = true;
  sample.joint_order_token = "tok";
  sample.source_identity = "predicted_state";
  sample.authority_profile = "native_predicted_v1";
  sample.measured_authority = false;
  EXPECT_FALSE(tracker.update(sample, "tok", policy, 10));
  EXPECT_FALSE(tracker.evaluate(policy, 10));

  sample.sequence = 2;
  sample.source_identity = "joint_state";
  sample.authority_profile = "ros2_control_measured_v1";
  sample.measured_authority = true;
  EXPECT_TRUE(tracker.update(sample, "tok", policy, 10));
  EXPECT_TRUE(tracker.evaluate(policy, 10));
}

TEST(ObservedStateEvidenceTest, PreservesPublicationAuthorityClassAndProfile) {
  const std::vector<std::string> joints{"j1"};
  const std::vector<double> positions{0.5};
  const std::vector<double> velocities{0.0};

  ObservedRobotStatePublication predicted(
      joints, {{ObservedRobotStateSourceKind::kPredictedState, "predicted",
                ObservedRobotStateAuthorityProfile::kPredictedNative, 0, "native_predicted_v1"}});
  ASSERT_TRUE(predicted
                  .publish(ObservedRobotStateSourceKind::kPredictedState, joints, positions,
                           velocities, 10, 10)
                  .published);
  const auto predicted_evidence = capture_observed_state_evidence(&predicted, 10, 0.01);
  ASSERT_TRUE(predicted_evidence.has_value());
  EXPECT_FALSE(predicted_evidence->measured_authority);
  EXPECT_EQ(predicted_evidence->authority_profile, "native_predicted_v1");

  ObservedRobotStatePublication measured(
      joints, {{ObservedRobotStateSourceKind::kJointState, "joint_state",
                ObservedRobotStateAuthorityProfile::kMeasured, 100, "ros2_control_measured_v1"}});
  ASSERT_TRUE(
      measured
          .publish(ObservedRobotStateSourceKind::kJointState, joints, positions, velocities, 10, 10)
          .published);
  const auto measured_evidence = capture_observed_state_evidence(&measured, 10, 0.01);
  ASSERT_TRUE(measured_evidence.has_value());
  EXPECT_TRUE(measured_evidence->measured_authority);
  EXPECT_EQ(measured_evidence->authority_profile, "ros2_control_measured_v1");
}

RecoveryAuthorityCapture make_interrupted_capture(InterruptPreemptionResult result,
                                                  bool owner_active,
                                                  bool with_matching_certificate) {
  RecoveryAuthorityCapture capture;
  capture.control_command_epoch = 3;
  capture.execution_id = 42;
  capture.execution_state = ExecutionState::kInterrupted;
  capture.active_latch = make_collision_latch("latch-1", 42, result);
  capture.fault_preempt_stop_active = owner_active;
  capture.fault_preempt_owner_generation = 5;
  if (with_matching_certificate) {
    capture.fault_preempt_certificate =
        make_certificate(StopOwnerKind::kFaultPreemptWorker, 5, 42, "latch-1");
  }
  return capture;
}

TEST(RecoveryAssessmentTest, PendingPreemptIsWaitingNotCertified) {
  const auto assessment = build_recovery_assessment(
      make_interrupted_capture(InterruptPreemptionResult::kPending, true, false),
      RecoveryReadinessPolicy{}, 0);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(assessment.reason_code, reason_codes::kRecoveryStopNotCertified);
  EXPECT_FALSE(assessment.retry_admissible);
}

TEST(RecoveryAssessmentTest, SucceededButOwnerActiveIsWaiting) {
  const auto assessment = build_recovery_assessment(
      make_interrupted_capture(InterruptPreemptionResult::kSucceeded, true, true),
      RecoveryReadinessPolicy{}, 0);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_FALSE(assessment.retry_admissible);
}

TEST(RecoveryAssessmentTest, LegacyFaultWorkerActiveIsWaiting) {
  auto capture = make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, true);
  capture.fault_preempt_worker_active = true;
  const auto assessment = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 0);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(assessment.reason_code, reason_codes::kRecoveryStopNotCertified);
  EXPECT_FALSE(assessment.retry_admissible);
}

TEST(RecoveryAssessmentTest, FailedPreemptNeverRetryReady) {
  const auto assessment = build_recovery_assessment(
      make_interrupted_capture(InterruptPreemptionResult::kBackendPreemptFailed, false, true),
      RecoveryReadinessPolicy{}, 0);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(assessment.reason_code, reason_codes::kRecoveryStopFailedEscalating);
  EXPECT_FALSE(assessment.retry_admissible);
}

TEST(RecoveryAssessmentTest, CertifiedQuiescentIsRetryReady) {
  const auto assessment = build_recovery_assessment(
      make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, true),
      RecoveryReadinessPolicy{}, 7);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kRetryReady);
  EXPECT_TRUE(assessment.retry_admissible);
  EXPECT_EQ(assessment.assessment_revision, 8u);
  ASSERT_EQ(assessment.allowed_actions.size(), 2u);
  EXPECT_EQ(assessment.allowed_actions[0], RecoveryAction::kRetryMotionFromCurrentState);
}

TEST(RecoveryAssessmentTest, PublicationHelperOwnsSemanticDecisionAndRecordMapping) {
  KernelRecoveryAssessmentDTO previous;
  auto capture = make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, true);
  const auto publication = prepare_recovery_assessment_publication(
      previous, capture, RecoveryReadinessPolicy{}, "unit_test_trigger");

  EXPECT_TRUE(publication.changed);
  EXPECT_EQ(publication.candidate.status, RecoveryAssessmentStatus::kRetryReady);
  EXPECT_EQ(publication.event_details.at("assessment_revision"), "1");
  EXPECT_EQ(publication.event_details.at("recovery_class"), "runtime_collision");
  EXPECT_EQ(publication.event_details.at("status"), "retry_ready");
  EXPECT_EQ(publication.event_details.at("retry_admissible"), "true");
  EXPECT_EQ(publication.event_details.at("trigger"), "unit_test_trigger");
}

TEST(RecoveryAssessmentTest, PublicationHelperDoesNotChurnEquivalentAssessmentRecord) {
  const auto capture = make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, true);
  const auto previous = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 7);
  const auto publication = prepare_recovery_assessment_publication(
      previous, capture, RecoveryReadinessPolicy{}, "equivalent_refresh");

  EXPECT_FALSE(publication.changed);
  EXPECT_TRUE(publication.event_details.empty());
  // Candidate carries the refreshed freshness facts/revision proposal; the
  // kernel deliberately retains `previous` as the published assessment.
  EXPECT_EQ(publication.candidate.assessment_revision, 9u);
}

TEST(RecoveryAssessmentTest, CertificateFromOtherExecutionIsNotCertification) {
  auto capture = make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, false);
  capture.fault_preempt_certificate =
      make_certificate(StopOwnerKind::kFaultPreemptWorker, 5, 99, "latch-1");
  const auto assessment = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 0);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(assessment.reason_code, reason_codes::kRecoveryStopNotCertified);
}

TEST(RecoveryAssessmentTest, MeasuredObservationRequiredWithoutProviderFailsClosed) {
  RecoveryReadinessPolicy policy;
  policy.require_measured_observation = true;
  const auto assessment = build_recovery_assessment(
      make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, true), policy, 0);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(assessment.reason_code, reason_codes::kRecoveryObservedStateUnavailable);
  EXPECT_FALSE(assessment.retry_admissible);
}

TEST(RecoveryAssessmentTest, MeasuredObservationRequiredRejectsPredictedAtRestEvidence) {
  RecoveryReadinessPolicy policy;
  policy.require_measured_observation = true;
  auto capture = make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, true);
  ObservedStateEvidenceDTO observed;
  observed.sequence = 1;
  observed.complete = true;
  observed.at_rest = true;
  observed.sampled_at_ns = 1;
  observed.freshness_budget_ns = 100;
  observed.source_identity = "predicted_state";
  observed.authority_profile = "native_predicted_v1";
  observed.measured_authority = false;
  capture.observed_state = observed;
  capture.observed_state_qualified = false;

  const auto assessment = build_recovery_assessment(capture, policy, 0);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(assessment.reason_code, reason_codes::kRecoveryObservedStateAuthorityNotMeasured);
  EXPECT_FALSE(assessment.retry_admissible);
  EXPECT_FALSE(assessment.observed_state_measured);
  EXPECT_EQ(assessment.observed_state_authority_profile, "native_predicted_v1");
}

TEST(RecoveryAssessmentTest, UnboundedObservationFreshnessHasNoAbsoluteDeadline) {
  auto capture = make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, true);
  ObservedStateEvidenceDTO observed;
  observed.sequence = 1;
  observed.complete = true;
  observed.at_rest = true;
  observed.sampled_at_ns = 123;
  observed.freshness_budget_ns = std::numeric_limits<std::int64_t>::max() - observed.sampled_at_ns;
  observed.source_identity = "native_predicted_state";
  observed.authority_profile = "retained_native_state_authority";
  capture.observed_state = observed;
  capture.observed_state_qualified = true;

  const auto assessment = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 0);

  EXPECT_TRUE(assessment.observed_state_available);
  EXPECT_TRUE(assessment.observed_state_fresh);
  EXPECT_EQ(assessment.observed_state_valid_until_ns, 0);
}

TEST(RecoveryAssessmentTest, EquallySafeFactsDoNotChurnRevision) {
  const auto capture = make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, true);
  const auto first = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 0);
  const auto second = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 1);
  EXPECT_FALSE(recovery_assessment_semantically_differs(first, second));
}

TEST(RecoveryAssessmentTest, AuthorityIdentityChangesAdvanceRevision) {
  auto first_capture = make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, true);
  auto second_capture = first_capture;
  second_capture.active_latch->source_id = "changed_source";
  const auto first = build_recovery_assessment(first_capture, RecoveryReadinessPolicy{}, 0);
  const auto second = build_recovery_assessment(second_capture, RecoveryReadinessPolicy{}, 1);
  EXPECT_TRUE(recovery_assessment_semantically_differs(first, second));
}

TEST(RecoveryAssessmentTest, ObservationSourceChangeAdvancesRevision) {
  RecoveryReadinessPolicy policy;
  policy.require_measured_observation = true;
  auto first_capture = make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, true);
  ObservedStateEvidenceDTO observed;
  observed.sequence = 1;
  observed.complete = true;
  observed.at_rest = true;
  observed.sampled_at_ns = 1;
  observed.freshness_budget_ns = 100;
  observed.source_identity = "controller_state";
  observed.authority_profile = "ros2_control_measured_v1";
  observed.measured_authority = true;
  first_capture.observed_state = observed;
  first_capture.observed_state_qualified = true;
  auto second_capture = first_capture;
  second_capture.observed_state->source_identity = "joint_state";
  const auto first = build_recovery_assessment(first_capture, policy, 0);
  const auto second = build_recovery_assessment(second_capture, policy, 1);
  EXPECT_TRUE(recovery_assessment_semantically_differs(first, second));
}

KernelRecoveryActionRequestDTO make_bound_request(const KernelRecoveryAssessmentDTO& assessment) {
  KernelRecoveryActionRequestDTO request;
  request.request_id = "req-1";
  request.action = assessment.allowed_actions.empty() ? RecoveryAction::kRetryMotionFromCurrentState
                                                      : assessment.allowed_actions.front();
  request.principal_id = "operator_test";
  request.expected_assessment_revision = assessment.assessment_revision;
  request.expected_control_command_epoch = assessment.control_command_epoch;
  request.expected_execution_id = assessment.execution_id;
  request.expected_latch_id = assessment.latch_id;
  request.expected_stop_certificate_id = assessment.stop_certificate_id;
  request.expected_stop_owner_generation = assessment.stop_owner_generation;
  return request;
}

TEST(RecoveryAdmissionTest, BoundRequestAdmitsAndStaleFieldsReject) {
  const auto capture = make_interrupted_capture(InterruptPreemptionResult::kSucceeded, false, true);
  const auto assessment = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 0);

  auto request = make_bound_request(assessment);
  EXPECT_TRUE(evaluate_recovery_admission(request, capture, assessment).admitted);

  auto stale_revision = request;
  stale_revision.expected_assessment_revision += 1;
  EXPECT_EQ(evaluate_recovery_admission(stale_revision, capture, assessment).rejection_reason,
            reason_codes::kRecoveryStaleAssessment);

  auto stale_epoch = request;
  stale_epoch.expected_control_command_epoch += 1;
  EXPECT_EQ(evaluate_recovery_admission(stale_epoch, capture, assessment).rejection_reason,
            reason_codes::kRecoveryStaleEpoch);

  auto wrong_latch = request;
  wrong_latch.expected_latch_id = "latch-OLD";
  EXPECT_EQ(evaluate_recovery_admission(wrong_latch, capture, assessment).rejection_reason,
            reason_codes::kRecoveryLatchChanged);

  auto wrong_certificate = request;
  wrong_certificate.expected_stop_certificate_id = "cert-old";
  EXPECT_EQ(evaluate_recovery_admission(wrong_certificate, capture, assessment).rejection_reason,
            reason_codes::kRecoveryStopCertificateChanged);

  auto wrong_generation = request;
  wrong_generation.expected_stop_owner_generation += 1;
  EXPECT_EQ(evaluate_recovery_admission(wrong_generation, capture, assessment).rejection_reason,
            reason_codes::kRecoveryStopOwnerGenerationChanged);

  auto no_principal = request;
  no_principal.principal_id.clear();
  EXPECT_EQ(evaluate_recovery_admission(no_principal, capture, assessment).rejection_reason,
            reason_codes::kRecoveryPrincipalMissing);
}

TEST(RecoveryAdmissionTest, WaitingAssessmentRejectsAction) {
  const auto capture = make_interrupted_capture(InterruptPreemptionResult::kPending, true, false);
  const auto assessment = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 0);
  auto request = make_bound_request(assessment);
  request.action = RecoveryAction::kRetryMotionFromCurrentState;
  const auto decision = evaluate_recovery_admission(request, capture, assessment);
  EXPECT_FALSE(decision.admitted);
  EXPECT_EQ(decision.rejection_reason, reason_codes::kRecoveryStopNotCertified);
}

TEST(RecoveryAssessmentTest, PausedPublishesNoRecoveryOffer) {
  RecoveryAuthorityCapture capture;
  capture.execution_state = ExecutionState::kPaused;
  const auto assessment = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 0);
  EXPECT_EQ(assessment.recovery_class, RecoveryClass::kNone);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kUnavailable);
  EXPECT_TRUE(assessment.allowed_actions.empty());
}

TEST(RecoveryAssessmentTest, StoppedWithCertificateOffersReplan) {
  RecoveryAuthorityCapture capture;
  capture.control_command_epoch = 2;
  capture.execution_id = 7;
  capture.execution_state = ExecutionState::kStopped;
  capture.operator_stop_certificate =
      make_certificate(StopOwnerKind::kOperatorStopWorker, 3, 7, "");
  capture.operator_stop_owner_generation = 3;
  const auto assessment = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 0);
  EXPECT_EQ(assessment.recovery_class, RecoveryClass::kOrdinaryOperatorStop);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kRetryReady);
  ASSERT_EQ(assessment.allowed_actions.size(), 2u);
  EXPECT_EQ(assessment.allowed_actions[0], RecoveryAction::kReplanFromStoppedState);
}

TEST(RecoveryAssessmentTest,
     StoppedWithExactAddressedSettlementOffersReplanWithoutLocalCertificate) {
  RecoveryAuthorityCapture capture;
  capture.control_command_epoch = 2;
  capture.execution_id = 7;
  capture.execution_state = ExecutionState::kStopped;
  capture.operator_stop_owner_generation = 3;
  capture.addressed_motion_stop_evidence = AddressedMotionStopRecoveryEvidenceDTO{
      7, 3,
      StopCertificateRefDTO{StopCertificateId("aggregate-certificate.1"),
                            StopCertificateRevision(1U), StopId("stop.1"), StopGeneration(4U)},
      true};

  const auto assessment = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 0);

  EXPECT_EQ(assessment.recovery_class, RecoveryClass::kOrdinaryOperatorStop);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kRetryReady);
  EXPECT_TRUE(assessment.retry_admissible);
  EXPECT_TRUE(assessment.backend_stop_confirmed);
  EXPECT_EQ(assessment.stop_certificate_id, "aggregate-certificate.1");
  EXPECT_EQ(assessment.stop_certificate_revision, 1U);
  EXPECT_EQ(assessment.stop_owner_generation, 3U);
  ASSERT_EQ(assessment.evidence_refs.size(), 1U);
  EXPECT_EQ(assessment.evidence_refs.front(), "aggregate-certificate.1");
  ASSERT_EQ(assessment.allowed_actions.size(), 2U);
  EXPECT_EQ(assessment.allowed_actions[0], RecoveryAction::kReplanFromStoppedState);
}

TEST(RecoveryAssessmentTest, StoppedRejectsAddressedSettlementFromAnotherLocalStopOwnerGeneration) {
  RecoveryAuthorityCapture capture;
  capture.execution_id = 7;
  capture.execution_state = ExecutionState::kStopped;
  capture.operator_stop_owner_generation = 3;
  capture.addressed_motion_stop_evidence = AddressedMotionStopRecoveryEvidenceDTO{
      7, 2,
      StopCertificateRefDTO{StopCertificateId("aggregate-certificate.1"),
                            StopCertificateRevision(1U), StopId("stop.1"), StopGeneration(4U)},
      true};

  const auto assessment = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 0);

  EXPECT_EQ(assessment.recovery_class, RecoveryClass::kOrdinaryOperatorStop);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(assessment.reason_code, reason_codes::kRecoveryStopNotCertified);
  EXPECT_FALSE(assessment.retry_admissible);
}

TEST(RecoveryAssessmentTest, StoppedWithUnconfirmedCertificateRemainsFailClosed) {
  RecoveryAuthorityCapture capture;
  capture.execution_id = 7;
  capture.execution_state = ExecutionState::kStopped;
  auto certificate = make_certificate(StopOwnerKind::kOperatorStopWorker, 3, 7, "");
  certificate.completion_result = StopCertificateOutcome::kPartial;
  certificate.backend_stop_confirmed = false;
  capture.operator_stop_certificate = std::move(certificate);
  capture.operator_stop_owner_generation = 3;

  const auto assessment = build_recovery_assessment(capture, RecoveryReadinessPolicy{}, 0);
  EXPECT_EQ(assessment.recovery_class, RecoveryClass::kOrdinaryOperatorStop);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(assessment.reason_code, reason_codes::kRecoveryStopNotCertified);
  EXPECT_FALSE(assessment.retry_admissible);
  EXPECT_EQ(assessment.allowed_actions.size(), 1u);
  EXPECT_EQ(assessment.allowed_actions.front(), RecoveryAction::kResetCycle);
}

TEST(RecoveryAssessmentTest, StrongerAuthorityRevokesStoppedAndFailedOffers) {
  RecoveryAuthorityCapture stopped;
  stopped.execution_id = 7;
  stopped.execution_state = ExecutionState::kStopped;
  stopped.operator_stop_owner_generation = 3;
  stopped.operator_stop_certificate =
      make_certificate(StopOwnerKind::kOperatorStopWorker, 3, 7, "");
  stopped.stronger_authority_active = true;
  const auto stopped_assessment = build_recovery_assessment(stopped, RecoveryReadinessPolicy{}, 0);
  EXPECT_EQ(stopped_assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(stopped_assessment.reason_code, reason_codes::kRecoveryStrongerAuthorityActive);
  EXPECT_FALSE(stopped_assessment.retry_admissible);

  RecoveryAuthorityCapture failed;
  failed.execution_id = 8;
  failed.execution_state = ExecutionState::kFailed;
  failed.failed_validation_retryable = true;
  failed.queued_latch_present = true;
  const auto failed_assessment = build_recovery_assessment(failed, RecoveryReadinessPolicy{}, 0);
  EXPECT_EQ(failed_assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(failed_assessment.reason_code, reason_codes::kRecoveryQueuedLatchPresent);
  EXPECT_FALSE(failed_assessment.retry_admissible);
}

}  // namespace
}  // namespace manyforge::core
