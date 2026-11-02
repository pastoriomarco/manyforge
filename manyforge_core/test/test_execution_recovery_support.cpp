#include <gtest/gtest.h>

#include <limits>
#include <manyforge_core/execution_recovery_support.hpp>
#include <manyforge_core/types/reason_codes.hpp>

namespace manyforge::core {
namespace {

KernelStopCertificateDTO make_certificate(StopOwnerKind owner, std::uint64_t generation,
                                          std::uint64_t execution_id) {
  KernelStopCertificateDTO certificate;
  certificate.certificate_id = "cert-g" + std::to_string(generation);
  certificate.completion_result = StopCertificateOutcome::kSucceeded;
  certificate.stop_owner = owner;
  certificate.stop_owner_generation = generation;
  certificate.execution_id = execution_id;
  certificate.backend_stop_confirmed = true;
  certificate.at_rest_evidence = AtRestEvidence::kUnavailable;
  return certificate;
}

TEST(RecoveryCertificateStoreTest, LateOldGenerationCannotOverwriteNewer) {
  RecoveryCertificateStore store;
  store.publish(make_certificate(StopOwnerKind::kFaultPreemptWorker, 7, 42));
  // Late completion from an OLDER owner generation arrives afterwards.
  store.publish(make_certificate(StopOwnerKind::kFaultPreemptWorker, 6, 41));

  const auto authoritative = store.authoritative(StopOwnerKind::kFaultPreemptWorker);
  ASSERT_TRUE(authoritative.has_value());
  EXPECT_EQ(authoritative->stop_owner_generation, 7u);
  EXPECT_EQ(authoritative->execution_id, 42u);
  // Exact-key admission lookup for the stale generation fails.
  EXPECT_FALSE(store.find_authoritative(StopOwnerKind::kFaultPreemptWorker, 41, 6).has_value());
}

TEST(RecoveryCertificateStoreTest, EqualGenerationDifferentIdentityCannotReplaceAuthority) {
  RecoveryCertificateStore store;
  store.publish(make_certificate(StopOwnerKind::kFaultPreemptWorker, 7, 42));
  store.publish(make_certificate(StopOwnerKind::kFaultPreemptWorker, 7, 99));
  const auto authoritative = store.authoritative(StopOwnerKind::kFaultPreemptWorker);
  ASSERT_TRUE(authoritative.has_value());
  EXPECT_EQ(authoritative->execution_id, 42u);
}

TEST(RecoveryCertificateStoreTest, OwnerSessionProgressUpgradesButNeverDuplicatesOrDowngrades) {
  RecoveryCertificateStore store;
  auto partial = make_certificate(StopOwnerKind::kOperatorStopWorker, 4, 17);
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
  EXPECT_EQ(store.last_revision(), 2u);
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

/// A certified operator Stop whose cleanup finished: retry-ready.
RecoveryAuthorityCapture make_stopped_capture() {
  RecoveryAuthorityCapture capture;
  capture.control_command_epoch = 3;
  capture.execution_id = 42;
  capture.execution_state = ExecutionState::kStopped;
  capture.operator_stop_owner_generation = 5;
  capture.operator_stop_certificate =
      make_certificate(StopOwnerKind::kOperatorStopWorker, 5, 42);
  capture.terminal_cleanup_complete = true;
  return capture;
}

RecoveryAuthorityCapture make_automatic_planning_capture(const std::string& code) {
  RecoveryAuthorityCapture capture;
  capture.execution_id = 42;
  capture.execution_state = ExecutionState::kFailed;
  capture.failed_validation_retryable = true;
  capture.failed_validation_code = code;
  capture.failed_execution_effect = ParticipantEffectKnowledge::kNoEffect;
  capture.failed_preparation_never_dispatched = true;
  return capture;
}

TEST(RecoveryAssessmentTest, AutomaticPlanningRetryRequiresClosedCauseAndExactNoEffectProof) {
  for (const auto* code : {kErrorPlanningFailed, kErrorPlanningTimeout, kErrorIkFailed,
                           kErrorCartesianIncomplete, kErrorValidationCollision}) {
    SCOPED_TRACE(code);
    auto capture = make_automatic_planning_capture(code);
    capture.failed_validation_conclusive_collision = true;
    const auto eligible = build_recovery_assessment(capture, 0);
    EXPECT_TRUE(eligible.retry_admissible);
    EXPECT_TRUE(eligible.retry_eligibility.eligible);
    EXPECT_EQ(eligible.retry_eligibility.cause, MotionRetryCause::kPlanningNoEffect);
    EXPECT_EQ(eligible.retry_eligibility.effect, MotionRetryEffect::kKnownNoEffect);
    EXPECT_EQ(eligible.retry_eligibility.reason, MotionRetryIneligibilityReason::kNone);

    capture.failed_preparation_never_dispatched = false;
    const auto unproven = build_recovery_assessment(capture, 0);
    EXPECT_TRUE(unproven.retry_admissible);  // Existing manual repair remains available.
    EXPECT_FALSE(unproven.retry_eligibility.eligible);
    EXPECT_EQ(unproven.retry_eligibility.reason,
              MotionRetryIneligibilityReason::kDispatchAbsenceUnproven);

    capture.failed_preparation_never_dispatched = true;
    for (const auto effect : {ParticipantEffectKnowledge::kEffectSettled,
                              ParticipantEffectKnowledge::kEffectUncertain}) {
      capture.failed_execution_effect = effect;
      const auto effectful = build_recovery_assessment(capture, 0);
      EXPECT_FALSE(effectful.retry_eligibility.eligible);
      EXPECT_EQ(effectful.retry_eligibility.effect,
                effect == ParticipantEffectKnowledge::kEffectSettled
                    ? MotionRetryEffect::kEffectObserved
                    : MotionRetryEffect::kEffectUncertain);
    }
  }
}

TEST(RecoveryAssessmentTest, AutomaticRetryDoesNotInheritBroaderManualFailureRecovery) {
  for (const auto* code : {kErrorContactPolicyViolation, kErrorSceneChangedDuringValidation,
                           kErrorExecutionRejected, kErrorValidationDeferred, "signal_trip",
                           "no_signal_trip", "interrupted_fault", "vendor_fault", "unknown"}) {
    SCOPED_TRACE(code);
    auto capture = make_automatic_planning_capture(code);
    capture.failed_validation_reason = "validation_failed";
    capture.failed_validation_conclusive_collision = true;
    const auto assessment = build_recovery_assessment(capture, 0);
    EXPECT_TRUE(assessment.retry_admissible);
    EXPECT_FALSE(assessment.retry_eligibility.eligible);
    EXPECT_EQ(assessment.retry_eligibility.reason,
              MotionRetryIneligibilityReason::kFailureCauseNotRetryable);
  }

  // The same validation_collision code also covers inconclusive/cancelled
  // validation. Only the retained typed authoritative collision can qualify.
  const auto inconclusive = build_recovery_assessment(
      make_automatic_planning_capture(kErrorValidationCollision), 0);
  EXPECT_TRUE(inconclusive.retry_admissible);
  EXPECT_FALSE(inconclusive.retry_eligibility.eligible);
  EXPECT_EQ(inconclusive.retry_eligibility.reason,
            MotionRetryIneligibilityReason::kValidationNotConclusiveCollision);
}

TEST(RecoveryAssessmentTest, AutomaticPlanningRetryIsRevokedByExistingAuthorities) {
  const auto check = [](RecoveryAuthorityCapture capture, MotionRetryIneligibilityReason reason) {
    const auto assessment = build_recovery_assessment(capture, 0);
    EXPECT_FALSE(assessment.retry_eligibility.eligible);
    EXPECT_EQ(assessment.retry_eligibility.reason, reason);
  };
  auto capture = make_automatic_planning_capture(kErrorPlanningFailed);
  capture.operator_stop_active = true;
  check(capture, MotionRetryIneligibilityReason::kStopOwnerActive);
  capture.operator_stop_active = false;
  capture.stronger_authority_active = true;
  check(capture, MotionRetryIneligibilityReason::kStrongerAuthorityActive);
}

TEST(RecoveryAssessmentTest, AutomaticPlanningRetryDoesNotClassifyOtherExecutionStates) {
  for (const auto state :
       {ExecutionState::kIdle, ExecutionState::kPlanning, ExecutionState::kValidating,
        ExecutionState::kExecuting, ExecutionState::kCompleted, ExecutionState::kStopped,
        ExecutionState::kInterrupted, ExecutionState::kInterventionRequired}) {
    auto capture = make_automatic_planning_capture(kErrorPlanningFailed);
    capture.execution_state = state;
    const auto assessment = build_recovery_assessment(capture, 0);
    EXPECT_EQ(assessment.retry_eligibility, MotionRetryEligibilityDTO{});
  }
  EXPECT_FALSE(KernelRecoveryActionRequestDTO{}.fresh_motion_attempt);
}

TEST(RecoveryAssessmentTest, AutomaticEligibilityChangesAdvanceSemanticRevision) {
  auto capture = make_automatic_planning_capture(kErrorValidationCollision);
  const auto previous = build_recovery_assessment(capture, 7);
  capture.failed_validation_conclusive_collision = true;
  const auto publication = prepare_recovery_assessment_publication(
      previous, capture, "typed_collision_decision");
  EXPECT_TRUE(publication.changed);
  EXPECT_GT(publication.candidate.assessment_revision, previous.assessment_revision);
  EXPECT_EQ(publication.event_details.at("automatic_retry_eligible"), "true");
  EXPECT_EQ(publication.event_details.at("automatic_retry_cause"), "planning_no_effect");
  EXPECT_EQ(publication.event_details.at("automatic_retry_effect"), "known_no_effect");
  EXPECT_EQ(publication.event_details.at("automatic_retry_reason"), "none");
  EXPECT_FALSE(prepare_recovery_assessment_publication(publication.candidate, capture,
                                                       "same_decision")
                   .changed);
}

TEST(RecoveryAssessmentTest, AutomaticRetryWireVocabularyIsClosed) {
  EXPECT_STREQ(motion_retry_cause_name(MotionRetryCause::kPlanningNoEffect), "planning_no_effect");
  EXPECT_STREQ(motion_retry_cause_name(MotionRetryCause::kCollisionStopNominal),
               "collision_stop_nominal");
  EXPECT_STREQ(motion_retry_cause_name(MotionRetryCause::kNotEligible), "not_eligible");
  EXPECT_STREQ(motion_retry_effect_name(MotionRetryEffect::kKnownNoEffect), "known_no_effect");
  EXPECT_STREQ(motion_retry_effect_name(MotionRetryEffect::kEffectObserved), "effect_observed");
  EXPECT_STREQ(motion_retry_effect_name(MotionRetryEffect::kEffectUncertain), "effect_uncertain");
  EXPECT_STREQ(motion_retry_ineligibility_reason_name(MotionRetryIneligibilityReason::kNone),
               "none");
}

TEST(RecoveryAssessmentTest, InterruptedRequiresReset) {
  RecoveryAuthorityCapture capture;
  capture.execution_id = 42;
  capture.execution_state = ExecutionState::kInterrupted;
  const auto assessment = build_recovery_assessment(capture, 0);
  EXPECT_EQ(assessment.recovery_class, RecoveryClass::kUnknownFault);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kResetRequired);
  EXPECT_FALSE(assessment.retry_admissible);
  ASSERT_EQ(assessment.allowed_actions.size(), 1u);
  EXPECT_EQ(assessment.allowed_actions[0], RecoveryAction::kResetCycle);
}

TEST(RecoveryAssessmentTest, PublicationHelperOwnsSemanticDecisionAndRecordMapping) {
  KernelRecoveryAssessmentDTO previous;
  auto capture = make_stopped_capture();
  const auto publication = prepare_recovery_assessment_publication(
      previous, capture, "unit_test_trigger");

  EXPECT_TRUE(publication.changed);
  EXPECT_EQ(publication.candidate.status, RecoveryAssessmentStatus::kRetryReady);
  EXPECT_EQ(publication.event_details.at("assessment_revision"), "1");
  EXPECT_EQ(publication.event_details.at("recovery_class"), "ordinary_operator_stop");
  EXPECT_EQ(publication.event_details.at("status"), "retry_ready");
  EXPECT_EQ(publication.event_details.at("retry_admissible"), "true");
  EXPECT_EQ(publication.event_details.at("trigger"), "unit_test_trigger");
}

TEST(RecoveryAssessmentTest, PublicationHelperDoesNotChurnEquivalentAssessmentRecord) {
  const auto capture = make_stopped_capture();
  const auto previous = build_recovery_assessment(capture, 7);
  const auto publication = prepare_recovery_assessment_publication(
      previous, capture, "equivalent_refresh");

  EXPECT_FALSE(publication.changed);
  EXPECT_TRUE(publication.event_details.empty());
  // Candidate carries the refreshed freshness facts/revision proposal; the
  // kernel deliberately retains `previous` as the published assessment.
  EXPECT_EQ(publication.candidate.assessment_revision, 9u);
}

TEST(RecoveryAssessmentTest, UnboundedObservationFreshnessHasNoAbsoluteDeadline) {
  auto capture = make_stopped_capture();
  ObservedStateEvidenceDTO observed;
  observed.sequence = 1;
  observed.complete = true;
  observed.at_rest = true;
  observed.sampled_at_ns = 123;
  observed.freshness_budget_ns = std::numeric_limits<std::int64_t>::max() - observed.sampled_at_ns;
  observed.source_identity = "native_predicted_state";
  observed.authority_profile = "retained_state_authority";
  capture.observed_state = observed;
  capture.observed_state_qualified = true;

  const auto assessment = build_recovery_assessment(capture, 0);

  EXPECT_TRUE(assessment.observed_state_available);
  EXPECT_TRUE(assessment.observed_state_fresh);
  EXPECT_EQ(assessment.observed_state_valid_until_ns, 0);
}

TEST(RecoveryAssessmentTest, EquallySafeFactsDoNotChurnRevision) {
  const auto capture = make_stopped_capture();
  const auto first = build_recovery_assessment(capture, 0);
  const auto second = build_recovery_assessment(capture, 1);
  EXPECT_FALSE(recovery_assessment_semantically_differs(first, second));
}

TEST(RecoveryAssessmentTest, AuthorityIdentityChangesAdvanceRevision) {
  auto first_capture = make_stopped_capture();
  auto second_capture = first_capture;
  second_capture.operator_stop_certificate->certificate_id = "changed_certificate";
  const auto first = build_recovery_assessment(first_capture, 0);
  const auto second = build_recovery_assessment(second_capture, 1);
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
  request.expected_stop_certificate_id = assessment.stop_certificate_id;
  request.expected_stop_owner_generation = assessment.stop_owner_generation;
  return request;
}

TEST(RecoveryAdmissionTest, BoundRequestAdmitsAndStaleFieldsReject) {
  const auto capture = make_stopped_capture();
  const auto assessment = build_recovery_assessment(capture, 0);

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
  auto capture = make_stopped_capture();
  capture.operator_stop_certificate.reset();
  const auto assessment = build_recovery_assessment(capture, 0);
  auto request = make_bound_request(assessment);
  request.action = RecoveryAction::kReplanFromStoppedState;
  const auto decision = evaluate_recovery_admission(request, capture, assessment);
  EXPECT_FALSE(decision.admitted);
  EXPECT_EQ(decision.rejection_reason, reason_codes::kRecoveryStopNotCertified);
}

TEST(RecoveryAssessmentTest, StoppedWithCertificateOffersReplan) {
  RecoveryAuthorityCapture capture;
  capture.control_command_epoch = 2;
  capture.execution_id = 7;
  capture.execution_state = ExecutionState::kStopped;
  capture.operator_stop_certificate =
      make_certificate(StopOwnerKind::kOperatorStopWorker, 3, 7);
  capture.operator_stop_owner_generation = 3;
  capture.terminal_cleanup_complete = true;
  const auto assessment = build_recovery_assessment(capture, 0);
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
  capture.terminal_cleanup_complete = true;

  const auto assessment = build_recovery_assessment(capture, 0);

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

// F-6a (W): a certified Stop whose execute worker's terminal cleanup is still
// pending is not yet decided: waiting, with its certificate, neither retry nor
// close. Cleanup completion republishes retry_ready with close permission.
TEST(RecoveryAssessmentTest, StoppedWaitsForTerminalCleanupBeforeRetryOrClose) {
  RecoveryAuthorityCapture capture;
  capture.control_command_epoch = 2;
  capture.execution_id = 7;
  capture.execution_state = ExecutionState::kStopped;
  capture.terminal_effect = ParticipantEffectKnowledge::kEffectSettled;
  capture.operator_stop_owner_generation = 3;
  capture.addressed_motion_stop_evidence = AddressedMotionStopRecoveryEvidenceDTO{
      7, 3,
      StopCertificateRefDTO{StopCertificateId("aggregate-certificate.1"),
                            StopCertificateRevision(1U), StopId("stop.1"), StopGeneration(4U)},
      true};

  const auto pending = build_recovery_assessment(capture, 0);
  EXPECT_EQ(pending.recovery_class, RecoveryClass::kOrdinaryOperatorStop);
  EXPECT_EQ(pending.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(pending.reason_code, reason_codes::kRecoveryTerminalCleanupPending);
  EXPECT_FALSE(pending.retry_admissible);
  EXPECT_FALSE(pending.eligible_close);
  EXPECT_TRUE(pending.backend_stop_confirmed);
  EXPECT_EQ(pending.stop_certificate_id, "aggregate-certificate.1");
  ASSERT_EQ(pending.allowed_actions.size(), 1U);
  EXPECT_EQ(pending.allowed_actions.front(), RecoveryAction::kResetCycle);
  EXPECT_TRUE(stopped_awaiting_terminal_cleanup(pending));
  auto replan = make_bound_request(pending);
  replan.action = RecoveryAction::kReplanFromStoppedState;
  EXPECT_FALSE(evaluate_recovery_admission(replan, capture, pending).admitted);

  capture.terminal_cleanup_complete = true;
  const auto ready =
      build_recovery_assessment(capture, pending.assessment_revision);
  EXPECT_EQ(ready.status, RecoveryAssessmentStatus::kRetryReady);
  EXPECT_TRUE(ready.retry_admissible);
  EXPECT_TRUE(ready.eligible_close);
  EXPECT_FALSE(stopped_awaiting_terminal_cleanup(ready));
  EXPECT_TRUE(recovery_assessment_semantically_differs(pending, ready));
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

  const auto assessment = build_recovery_assessment(capture, 0);

  EXPECT_EQ(assessment.recovery_class, RecoveryClass::kOrdinaryOperatorStop);
  EXPECT_EQ(assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(assessment.reason_code, reason_codes::kRecoveryStopNotCertified);
  EXPECT_FALSE(assessment.retry_admissible);
}

TEST(RecoveryAssessmentTest, StoppedWithUnconfirmedCertificateRemainsFailClosed) {
  RecoveryAuthorityCapture capture;
  capture.execution_id = 7;
  capture.execution_state = ExecutionState::kStopped;
  auto certificate = make_certificate(StopOwnerKind::kOperatorStopWorker, 3, 7);
  certificate.completion_result = StopCertificateOutcome::kPartial;
  certificate.backend_stop_confirmed = false;
  capture.operator_stop_certificate = std::move(certificate);
  capture.operator_stop_owner_generation = 3;

  const auto assessment = build_recovery_assessment(capture, 0);
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
      make_certificate(StopOwnerKind::kOperatorStopWorker, 3, 7);
  stopped.stronger_authority_active = true;
  const auto stopped_assessment = build_recovery_assessment(stopped, 0);
  EXPECT_EQ(stopped_assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(stopped_assessment.reason_code, reason_codes::kRecoveryStrongerAuthorityActive);
  EXPECT_FALSE(stopped_assessment.retry_admissible);

  RecoveryAuthorityCapture failed;
  failed.execution_id = 8;
  failed.execution_state = ExecutionState::kFailed;
  failed.failed_validation_retryable = true;
  failed.stronger_authority_active = true;
  const auto failed_assessment = build_recovery_assessment(failed, 0);
  EXPECT_EQ(failed_assessment.status, RecoveryAssessmentStatus::kWaiting);
  EXPECT_EQ(failed_assessment.reason_code, reason_codes::kRecoveryStrongerAuthorityActive);
  EXPECT_FALSE(failed_assessment.retry_admissible);
}

TEST(RecoveryAssessmentTest, GeneralCloseUsesTerminalEffectAndCleanupRatherThanRetryCause) {
  for (const auto effect :
       {ParticipantEffectKnowledge::kNoEffect, ParticipantEffectKnowledge::kEffectSettled,
        ParticipantEffectKnowledge::kEffectUncertain}) {
    RecoveryAuthorityCapture capture;
    capture.execution_id = 17U;
    capture.execution_state = ExecutionState::kFailed;
    capture.failed_validation_code = "non_retryable_operation_failure";
    capture.terminal_effect = effect;
    capture.terminal_cleanup_complete = true;
    const auto assessment = build_recovery_assessment(capture, 0U);
    EXPECT_FALSE(assessment.retry_eligibility.eligible);
    EXPECT_FALSE(assessment.retry_admissible);
    EXPECT_EQ(assessment.eligible_close, effect != ParticipantEffectKnowledge::kEffectUncertain);
    auto request = make_bound_request(assessment);
    request.action = RecoveryAction::kCloseOperation;
    EXPECT_EQ(evaluate_recovery_admission(request, capture, assessment).admitted,
              assessment.eligible_close);
    request.fresh_motion_attempt = true;
    EXPECT_FALSE(evaluate_recovery_admission(request, capture, assessment).admitted);
  }
}

TEST(RecoveryAssessmentTest, GeneralCloseWaitsForCleanupAndRetainsEveryAuthorityFence) {
  RecoveryAuthorityCapture capture;
  capture.execution_id = 19U;
  capture.execution_state = ExecutionState::kCompleted;
  capture.terminal_effect = ParticipantEffectKnowledge::kEffectSettled;
  EXPECT_FALSE(build_recovery_assessment(capture, 0U).eligible_close);
  capture.terminal_cleanup_complete = true;
  const auto ready = build_recovery_assessment(capture, 0U);
  ASSERT_TRUE(ready.eligible_close);
  EXPECT_FALSE(recovery_assessment_semantically_differs(ready, ready));
  auto request = make_bound_request(ready);
  request.action = RecoveryAction::kCloseOperation;
  EXPECT_TRUE(evaluate_recovery_admission(request, capture, ready).admitted);
  ++request.expected_execution_id;
  EXPECT_FALSE(evaluate_recovery_admission(request, capture, ready).admitted);
  for (const int fence : {0, 1}) {
    auto held = capture;
    held.operator_stop_active = fence == 0;
    held.stronger_authority_active = fence == 1;
    const auto assessment = build_recovery_assessment(held, ready.assessment_revision);
    EXPECT_FALSE(assessment.eligible_close);
    EXPECT_TRUE(recovery_assessment_semantically_differs(ready, assessment));
  }
}

}  // namespace
}  // namespace manyforge::core
