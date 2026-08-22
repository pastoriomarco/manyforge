#include <gtest/gtest.h>

#include <manyforge_core/types/error_code_mapping.hpp>

namespace manyforge::core {
namespace {

TEST(ErrorCodeMappingTest, MapsPublicErrorDtoCodes) {
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorStartStateMismatch, "", {}}),
            ErrorCode::kStartStateMismatch);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorMissingReference, "", {}}),
            ErrorCode::kMissingReference);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorAmbiguousReference, "", {}}),
            ErrorCode::kAmbiguousReference);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorStaleGeneration, "", {}}),
            ErrorCode::kStaleGeneration);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorAuthoringNotReady, "", {}}),
            ErrorCode::kAuthoringNotReady);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorValidationLimitsViolation, "", {}}),
            ErrorCode::kValidationLimitsViolation);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorValidationCollision, "", {}}),
            ErrorCode::kValidationCollision);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorInvalidContactPolicy, "", {}}),
            ErrorCode::kInvalidContactPolicy);
  EXPECT_EQ(
      error_code_from_dto(ErrorDTO{kErrorContactPolicyViolation, "", {}}),
      ErrorCode::kContactPolicyViolation);
  EXPECT_EQ(error_code_from_dto(
                ErrorDTO{kErrorSceneChangedDuringValidation, "", {}}),
            ErrorCode::kSceneChangedDuringValidation);
  EXPECT_EQ(error_code_from_dto(
                ErrorDTO{kErrorSceneObjectStateConflict, "", {}}),
            ErrorCode::kSceneObjectStateConflict);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorExecutionTimeout, "", {}}),
            ErrorCode::kExecutionTimeout);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorStopFailed, "", {}}),
            ErrorCode::kStopFailed);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorPauseFailed, "", {}}),
            ErrorCode::kPauseFailed);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorResumeFailed, "", {}}),
            ErrorCode::kResumeFailed);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorResetFailed, "", {}}),
            ErrorCode::kResetFailed);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorInvalidRequest, "", {}}),
            ErrorCode::kInvalidRequest);
  EXPECT_EQ(error_code_from_dto(ErrorDTO{kErrorInvariantViolation, "", {}}),
            ErrorCode::kInvariantViolation);
}

TEST(ErrorCodeMappingTest, MapsMotionInterruptCodes) {
  EXPECT_EQ(error_code_from_dto(
                ErrorDTO{kErrorMotionInterruptArmFailed, "", {}}),
            ErrorCode::kMotionInterruptArmFailed);
  EXPECT_EQ(error_code_from_dto(
                ErrorDTO{kErrorMotionInterruptSourceActiveAtArm, "", {}}),
            ErrorCode::kMotionInterruptSourceActiveAtArm);
  EXPECT_EQ(error_code_from_dto(
                ErrorDTO{kErrorMotionInterruptBackendStopFailed, "", {}}),
            ErrorCode::kMotionInterruptBackendStopFailed);
  EXPECT_EQ(error_code_from_dto(
                ErrorDTO{kErrorMotionInterruptStopStateInvalid, "", {}}),
            ErrorCode::kMotionInterruptStopStateInvalid);
}

TEST(ErrorCodeMappingTest, UnknownCodeFailsClosedToInternalError) {
  EXPECT_EQ(error_code_from_dto(ErrorDTO{"future_or_bad_code", "", {}}),
            ErrorCode::kInternalError);
}

}  // namespace
}  // namespace manyforge::core
