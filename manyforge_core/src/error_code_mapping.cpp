#include <manyforge_core/types/error_code_mapping.hpp>

namespace manyforge::core {

ErrorCode error_code_from_dto(const ErrorDTO& error) {
  if (error.code == kErrorStartStateMismatch) {
    return ErrorCode::kStartStateMismatch;
  }
  if (error.code == kErrorMissingReference) {
    return ErrorCode::kMissingReference;
  }
  if (error.code == kErrorAmbiguousReference) {
    return ErrorCode::kAmbiguousReference;
  }
  if (error.code == kErrorValidationLimitsViolation) {
    return ErrorCode::kValidationLimitsViolation;
  }
  if (error.code == kErrorValidationCollision) {
    return ErrorCode::kValidationCollision;
  }
  if (error.code == kErrorExecutionTimeout) {
    return ErrorCode::kExecutionTimeout;
  }
  if (error.code == kErrorStopFailed) {
    return ErrorCode::kStopFailed;
  }
  if (error.code == kErrorPauseFailed) {
    return ErrorCode::kPauseFailed;
  }
  if (error.code == kErrorResumeFailed) {
    return ErrorCode::kResumeFailed;
  }
  if (error.code == kErrorResetFailed) {
    return ErrorCode::kResetFailed;
  }
  if (error.code == kErrorInvalidRequest) {
    return ErrorCode::kInvalidRequest;
  }
  if (error.code == kErrorInvariantViolation) {
    return ErrorCode::kInvariantViolation;
  }
  if (error.code == kErrorMotionInterruptArmFailed) {
    return ErrorCode::kMotionInterruptArmFailed;
  }
  if (error.code == kErrorMotionInterruptSourceActiveAtArm) {
    return ErrorCode::kMotionInterruptSourceActiveAtArm;
  }
  if (error.code == kErrorMotionInterruptBackendStopFailed) {
    return ErrorCode::kMotionInterruptBackendStopFailed;
  }
  if (error.code == kErrorMotionInterruptStopStateInvalid) {
    return ErrorCode::kMotionInterruptStopStateInvalid;
  }
  return ErrorCode::kInternalError;
}

}  // namespace manyforge::core
