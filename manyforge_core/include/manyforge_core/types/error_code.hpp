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
    kValidationLimitsViolation,
    kObjectAlreadyExists,
    kObjectNotFound,
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
    kCollisionDetected
};

inline const char* to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::kSuccess: return "Success";
        case ErrorCode::kInvalidRequest: return "InvalidRequest";
        case ErrorCode::kMissingReference: return "MissingReference";
        case ErrorCode::kAmbiguousReference: return "AmbiguousReference";
        case ErrorCode::kUnsupportedCapability: return "UnsupportedCapability";
        case ErrorCode::kPlanningFailed: return "PlanningFailed";
        case ErrorCode::kPlanningTimeout: return "PlanningTimeout";
        case ErrorCode::kIkFailed: return "IkFailed";
        case ErrorCode::kCartesianIncomplete: return "CartesianIncomplete";
        case ErrorCode::kStartStateMismatch: return "StartStateMismatch";
        case ErrorCode::kValidationCollision: return "ValidationCollision";
        case ErrorCode::kValidationLimitsViolation: return "ValidationLimitsViolation";
        case ErrorCode::kObjectAlreadyExists: return "ObjectAlreadyExists";
        case ErrorCode::kObjectNotFound: return "ObjectNotFound";
        case ErrorCode::kExecutionRejected: return "ExecutionRejected";
        case ErrorCode::kExecutionAborted: return "ExecutionAborted";
        case ErrorCode::kExecutionTimeout: return "ExecutionTimeout";
        case ErrorCode::kStopFailed: return "StopFailed";
        case ErrorCode::kPauseFailed: return "PauseFailed";
        case ErrorCode::kResumeFailed: return "ResumeFailed";
        case ErrorCode::kResetFailed: return "ResetFailed";
        case ErrorCode::kInternalError: return "InternalError";
        case ErrorCode::kInvariantViolation: return "InvariantViolation";
        case ErrorCode::kInvalidStateTransition: return "InvalidStateTransition";
        case ErrorCode::kCacheMiss: return "CacheMiss";
        case ErrorCode::kCollisionDetected: return "CollisionDetected";
        default: return "Unknown";
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
    bool ok() const {
        return value_.has_value();
    }

    /// Get the value (throws if error)
    T& value() {
        if (!value_.has_value()) {
            throw std::runtime_error("Called value() on error Result");
        }
        return *value_;
    }

    const T& value() const {
        if (!value_.has_value()) {
            throw std::runtime_error("Called value() on error Result");
        }
        return *value_;
    }

    /// Get the error (throws if ok)
    ErrorDTO& error() {
        if (!error_.has_value()) {
            throw std::runtime_error("Called error() on ok Result");
        }
        return *error_;
    }

    const ErrorDTO& error() const {
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
    bool ok() const {
        return ok_;
    }

    /// Get the error (throws if ok)
    ErrorDTO& error() {
        if (ok_) {
            throw std::runtime_error("Called error() on ok Result");
        }
        return error_;
    }

    const ErrorDTO& error() const {
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

// Planning
inline const char* kErrorPlanningFailed = "planning_failed";
inline const char* kErrorPlanningTimeout = "planning_timeout";
inline const char* kErrorIkFailed = "ik_failed";
inline const char* kErrorCartesianIncomplete = "cartesian_incomplete";

// Validation (pre-execution gate)
inline const char* kErrorStartStateMismatch = "start_state_mismatch";
inline const char* kErrorValidationCollision = "validation_collision";
inline const char* kErrorValidationLimitsViolation = "validation_limits_violation";

// Scene editing
inline const char* kErrorObjectAlreadyExists = "object_already_exists";
inline const char* kErrorObjectNotFound = "object_not_found";

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

}  // namespace manyforge::core
