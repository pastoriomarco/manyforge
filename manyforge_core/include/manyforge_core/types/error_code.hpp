#pragma once

#include <map>
#include <string>
#include <variant>
#include <optional>
#include <stdexcept>

namespace manyforge::core {

/// Error DTO (spec §5.1)
struct ErrorDTO {
    std::string code;
    std::string message;
    std::map<std::string, std::string> details;
};

/// Error code enum (spec §5.2)
enum class ErrorCode {
    kSuccess,
    kInvalidRequest,
    kMissingReference,
    kAmbiguousReference,
    kUnsupportedCapability,
    kPlanningFailed,
    kPlanningTimeout,
    kIkFailed,
    kCartesianIncomplete,
    kStartStateMismatch,
    kValidationCollision,
    kInvalidContactPolicy,
    kContactPolicyViolation,
    kValidationDeferred,
    kValidationLimitsViolation,
    kObjectAlreadyExists,
    kObjectNotFound,
    kSceneObjectStateConflict,
    kExecutionRejected,
    kExecutionAborted,
    kExecutionTimeout,
    kStopFailed,
    kPauseFailed,
    kResumeFailed,
    kResetFailed,
    kInternalError,
    kInvariantViolation,
    kInvalidStateTransition,
    kCacheMiss,
    kCollisionDetected,
    kUnknownJointName,
    kSceneChangedDuringValidation,
    // Motion-scoped interrupt outcomes (spec/210 §5.2).
    kMotionInterruptArmFailed,
    kMotionInterruptSourceActiveAtArm,
    kMotionInterruptBackendStopFailed,
    kMotionInterruptStopStateInvalid
};

inline const char* to_string(ErrorCode code) {
    switch (code) {
        using enum ErrorCode;
        case kSuccess: return "success";
        case kInvalidRequest: return "invalid_request";
        case kMissingReference: return "missing_reference";
        case kAmbiguousReference: return "ambiguous_reference";
        case kUnsupportedCapability: return "unsupported_capability";
        case kPlanningFailed: return "planning_failed";
        case kPlanningTimeout: return "planning_timeout";
        case kIkFailed: return "ik_failed";
        case kCartesianIncomplete: return "cartesian_incomplete";
        case kStartStateMismatch: return "start_state_mismatch";
        case kValidationCollision: return "validation_collision";
        case kInvalidContactPolicy: return "invalid_contact_policy";
        case kContactPolicyViolation: return "contact_policy_violation";
        case kValidationDeferred: return "validation_deferred";
        case kValidationLimitsViolation: return "validation_limits_violation";
        case kObjectAlreadyExists: return "object_already_exists";
        case kObjectNotFound: return "object_not_found";
        case kSceneObjectStateConflict: return "scene_object_state_conflict";
        case kExecutionRejected: return "execution_rejected";
        case kExecutionAborted: return "execution_aborted";
        case kExecutionTimeout: return "execution_timeout";
        case kStopFailed: return "stop_failed";
        case kPauseFailed: return "pause_failed";
        case kResumeFailed: return "resume_failed";
        case kResetFailed: return "reset_failed";
        case kInternalError: return "internal_error";
        case kInvariantViolation: return "invariant_violation";
        case kInvalidStateTransition: return "invalid_state_transition";
        case kCacheMiss: return "cache_miss";
        case kCollisionDetected: return "collision_detected";
        case kUnknownJointName: return "unknown_joint_name";
        case kSceneChangedDuringValidation: return "scene_changed_during_validation";
        case kMotionInterruptArmFailed: return "motion_interrupt_arm_failed";
        case kMotionInterruptSourceActiveAtArm:
            return "motion_interrupt_source_active_at_arm";
        case kMotionInterruptBackendStopFailed:
            return "motion_interrupt_backend_stop_failed";
        case kMotionInterruptStopStateInvalid:
            return "motion_interrupt_stop_state_invalid";
        default: return "unknown";
    }
}

/// Result type for fallible operations (spec §5)
template <typename T>
struct Result {
private:
    std::optional<T> value_;
    std::optional<ErrorDTO> error_;

public:
    /// Create a successful result
    static Result Ok(const T& val) {
        Result r;
        r.value_ = val;
        return r;
    }

    static Result Ok(T&& val) {
        Result r;
        r.value_ = std::move(val);
        return r;
    }

    /// Create an error result
    static Result Err(const ErrorDTO& err) {
        Result r;
        r.error_ = err;
        return r;
    }

    static Result Err(ErrorDTO&& err) {
        Result r;
        r.error_ = std::move(err);
        return r;
    }

    /// Create an error result from ErrorCode
    static Result Err(ErrorCode code) {
        Result r;
        r.error_ = ErrorDTO{std::string(to_string(code)), "", {}};
        return r;
    }

    /// Check if result is ok
    [[nodiscard]] bool ok() const {
        return value_.has_value();
    }

    /// Get the value (throws if error)
    [[nodiscard]] T& value() {
        if (!value_.has_value()) {
            throw std::runtime_error("Called value() on error Result");
        }
        return *value_;
    }

    [[nodiscard]] const T& value() const {
        if (!value_.has_value()) {
            throw std::runtime_error("Called value() on error Result");
        }
        return *value_;
    }

    /// Get the error (throws if ok)
    [[nodiscard]] ErrorDTO& error() {
        if (!error_.has_value()) {
            throw std::runtime_error("Called error() on ok Result");
        }
        return *error_;
    }

    [[nodiscard]] const ErrorDTO& error() const {
        if (!error_.has_value()) {
            throw std::runtime_error("Called error() on ok Result");
        }
        return *error_;
    }

    /// Get value or default
    T value_or(T default_val) const {
        if (value_.has_value()) {
            return *value_;
        }
        return default_val;
    }
};

// Specialization for void results
template <>
struct Result<void> {
private:
    bool ok_;
    ErrorDTO error_;

public:
    /// Create a successful result
    static Result Ok() {
        Result r;
        r.ok_ = true;
        return r;
    }

    /// Create an error result
    static Result Err(const ErrorDTO& err) {
        Result r;
        r.ok_ = false;
        r.error_ = err;
        return r;
    }

    static Result Err(ErrorDTO&& err) {
        Result r;
        r.ok_ = false;
        r.error_ = std::move(err);
        return r;
    }

    /// Create an error result from ErrorCode
    static Result Err(ErrorCode code) {
        Result r;
        r.ok_ = false;
        r.error_ = ErrorDTO{std::string(to_string(code)), "", {}};
        return r;
    }

    /// Check if result is ok
    [[nodiscard]] bool ok() const {
        return ok_;
    }

    /// Get the error (throws if ok)
    [[nodiscard]] ErrorDTO& error() {
        if (ok_) {
            throw std::runtime_error("Called error() on ok Result");
        }
        return error_;
    }

    [[nodiscard]] const ErrorDTO& error() const {
        if (ok_) {
            throw std::runtime_error("Called error() on ok Result");
        }
        return error_;
    }
};

/// Error code string constants (spec §5.2)

// Request / capability
inline const char* kErrorInvalidRequest = "invalid_request";
inline const char* kErrorMissingReference = "missing_reference";
inline const char* kErrorAmbiguousReference = "ambiguous_reference";
inline const char* kErrorUnsupportedCapability = "unsupported_capability";
inline const char* kErrorRecoveryActionRejected = "recovery_action_rejected";

// Planning
inline const char* kErrorPlanningFailed = "planning_failed";
inline const char* kErrorPlanningTimeout = "planning_timeout";
inline const char* kErrorIkFailed = "ik_failed";
inline const char* kErrorCartesianIncomplete = "cartesian_incomplete";

// Validation (pre-execution gate)
inline const char* kErrorStartStateMismatch = "start_state_mismatch";
inline const char* kErrorValidationCollision = "validation_collision";
inline const char* kErrorInvalidContactPolicy = "invalid_contact_policy";
inline const char* kErrorContactPolicyViolation = "contact_policy_violation";
inline const char* kErrorValidationDeferred = "validation_deferred";
inline const char* kErrorValidationLimitsViolation = "validation_limits_violation";
inline const char* kErrorSceneChangedDuringValidation =
    "scene_changed_during_validation";

// Scene editing
inline const char* kErrorObjectAlreadyExists = "object_already_exists";
inline const char* kErrorObjectNotFound = "object_not_found";
inline const char* kErrorSceneObjectStateConflict =
    "scene_object_state_conflict";

// Execution
inline const char* kErrorExecutionRejected = "execution_rejected";
inline const char* kErrorExecutionAborted = "execution_aborted";
inline const char* kErrorExecutionTimeout = "execution_timeout";
inline const char* kErrorStopFailed = "stop_failed";
inline const char* kErrorPauseFailed = "pause_failed";
inline const char* kErrorResumeFailed = "resume_failed";
inline const char* kErrorResetFailed = "reset_failed";

// Internal
inline const char* kErrorInternalError = "internal_error";
inline const char* kErrorInvariantViolation = "invariant_violation";

// Motion-scoped interrupt outcomes (spec/210 §5.2).
// `motion_interrupt_arm_failed` — atomic arm-before-dispatch rejected
// at least one `MotionInterruptScopeDTO`; the execution never reached
// `Executing`. `motion_interrupt_source_active_at_arm` is the specific
// arm-failure reason for `require_inactive_at_arm`. The two
// backend/stop-state failures are emitted when a
// `complete_move_on_signal` trip cannot produce a validated
// `ExecutionStopSnapshotDTO`.
inline const char* kErrorMotionInterruptArmFailed =
    "motion_interrupt_arm_failed";
inline const char* kErrorMotionInterruptSourceActiveAtArm =
    "motion_interrupt_source_active_at_arm";
inline const char* kErrorMotionInterruptBackendStopFailed =
    "motion_interrupt_backend_stop_failed";
inline const char* kErrorMotionInterruptStopStateInvalid =
    "motion_interrupt_stop_state_invalid";

}  // namespace manyforge::core
