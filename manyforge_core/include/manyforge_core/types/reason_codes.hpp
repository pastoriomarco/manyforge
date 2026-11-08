#pragma once

namespace manyforge::core::reason_codes {

// Shared diagnostic reason-code vocabulary. Keep these values stable: they are
// stored in event details, failure archives, and UI/MCP incident payloads.

inline constexpr char kRawIkFailure[] = "raw_ik_failure";
inline constexpr char kCollisionAwareIkFailure[] =
    "collision_aware_ik_failure";
inline constexpr char kCollisionValidationFailure[] =
    "collision_validation_failure";
inline constexpr char kSupportContactRejection[] =
    "support_contact_rejection";
inline constexpr char kContactPolicyRejection[] =
    "contact_policy_rejection";
inline constexpr char kStartStateInCollision[] = "start_state_in_collision";
inline constexpr char kPreviewFailed[] = "preview_failed";

inline constexpr char kRuntimeContactGraspMonitor[] =
    "runtime_contact_grasp_monitor";
inline constexpr char kRuntimeContactGraspUnknownPolicy[] =
    "runtime_contact_grasp_unknown_policy";
inline constexpr char kRuntimeContactGraspInvalidPolicy[] =
    "runtime_contact_grasp_invalid_policy";
inline constexpr char kMissingGraspContactClearance[] =
    "missing_grasp_contact_clearance";
inline constexpr char kGraspContactPenetrationExceeded[] =
    "grasp_contact_penetration_exceeded";
inline constexpr char kUnknownContactPolicyId[] = "unknown_contact_policy_id";
inline constexpr char kInvalidContactGraspPolicy[] =
    "invalid_contact_grasp_policy";
inline constexpr char kInvalidContactGraspSampleFractions[] =
    "invalid_contact_grasp_sample_fractions";
inline constexpr char kMissingSignedClearance[] = "missing_signed_clearance";
inline constexpr char kInitialSampleInContact[] = "initial_sample_in_contact";
inline constexpr char kContactBeforeTerminalWindow[] =
    "contact_before_terminal_window";
inline constexpr char kPenetrationExceedsPolicy[] =
    "penetration_exceeds_policy";

inline constexpr char kCollisionValidationSerialFallbackException[] =
    "collision_validation_serial_fallback_exception";
inline constexpr char kCollisionValidationWorkerException[] =
    "collision_validation_worker_exception";
inline constexpr char kCollisionValidationSerialReferenceException[] =
    "collision_validation_serial_reference_exception";
inline constexpr char kEventSubscriberException[] =
    "event_subscriber_exception";
inline constexpr char kGripperStateUnavailable[] = "gripper_state_unavailable";
inline constexpr char kUnknownNonStdException[] = "unknown_non_std_exception";
inline constexpr char kTrackingSampleInvalid[] = "tracking_sample_invalid";
inline constexpr char kTerminalPublicationInProgress[] =
    "terminal_publication_in_progress";

// Recovery assessment/admission.
inline constexpr char kRecoveryNoneActive[] = "no_recovery_active";
inline constexpr char kRecoveryStopNotCertified[] = "stop_not_certified";
inline constexpr char kRecoveryStopCertified[] = "stop_certified";
inline constexpr char kRecoveryTerminalCleanupPending[] = "terminal_cleanup_pending";
inline constexpr char kRecoveryStopOwnerActive[] = "stop_owner_active";
inline constexpr char kRecoveryObservedStateUnavailable[] =
    "observed_state_unavailable";
inline constexpr char kRecoveryObservedStateStale[] = "observed_state_stale";
inline constexpr char kRecoveryObservedStateNotAtRest[] =
    "observed_state_not_at_rest";
inline constexpr char kRecoveryObservedStateAuthorityNotMeasured[] =
    "observed_state_authority_not_measured";
inline constexpr char kRecoveryStrongerAuthorityActive[] =
    "stronger_authority_active";
inline constexpr char kRecoveryLatchMissing[] = "recovery_latch_missing";
inline constexpr char kRecoveryLatchNotRetryable[] =
    "recovery_latch_not_retryable";
inline constexpr char kRecoveryInterventionChoiceRequired[] =
    "intervention_choice_required";
inline constexpr char kRecoveryFailedValidationRetryable[] =
    "failed_validation_retryable";
inline constexpr char kRecoveryFailedNotRetryable[] =
    "failed_not_retryable";
inline constexpr char kRecoveryStaleAssessment[] = "assessment_stale";
inline constexpr char kRecoveryStaleEpoch[] = "control_epoch_changed";
inline constexpr char kRecoveryExecutionChanged[] = "execution_changed";
inline constexpr char kRecoveryStopCertificateChanged[] =
    "stop_certificate_changed";
inline constexpr char kRecoveryStopOwnerGenerationChanged[] =
    "stop_owner_generation_changed";
inline constexpr char kRecoveryPreemptionNotSucceeded[] =
    "preemption_not_succeeded";
inline constexpr char kRecoveryActionNotAllowed[] = "action_not_allowed";
inline constexpr char kRecoveryPrincipalMissing[] =
    "principal_id_missing";
inline constexpr char kRecoveryRequestIdMissing[] =
    "request_id_missing";

}  // namespace manyforge::core::reason_codes
