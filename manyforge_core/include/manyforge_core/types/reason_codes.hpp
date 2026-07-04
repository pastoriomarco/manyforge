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

}  // namespace manyforge::core::reason_codes
