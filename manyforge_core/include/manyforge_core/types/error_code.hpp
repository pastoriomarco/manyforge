#pragma once

#include <string>
#include <variant>

namespace manyforge::core {

/// Error codes for fallible operations throughout manyforge_core.
/// ADR-0002: DTOs and error handling in core, no exceptions.
enum class ErrorCode {
  kSuccess = 0,

  // Planning errors (100-199)
  kPlanningFailed = 100,
  kPlanningTimeout = 101,
  kInvalidRequest = 102,
  kNoSolution = 103,
  kConstraintViolation = 104,

  // Execution errors (200-299)
  kExecutionFailed = 200,
  kExecutionTimeout = 201,
  kControllerUnavailable = 202,
  kTrajectoryRejected = 203,

  // Collision errors (300-399)
  kCollisionDetected = 300,
  kPreExecuteCollision = 301,

  // Scene errors (400-499)
  kSceneUnavailable = 400,
  kSceneStale = 401,

  // State errors (500-599)
  kInvalidStateTransition = 500,
  kNotIdle = 501,
  kAlreadyStopped = 502,

  // Cache errors (600-699)
  kCacheMiss = 600,

  // Internal errors (900-999)
  kInternalError = 900,
};

/// Convert ErrorCode to a human-readable string.
inline const char* to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::kSuccess: return "Success";
    case ErrorCode::kPlanningFailed: return "PlanningFailed";
    case ErrorCode::kPlanningTimeout: return "PlanningTimeout";
    case ErrorCode::kInvalidRequest: return "InvalidRequest";
    case ErrorCode::kNoSolution: return "NoSolution";
    case ErrorCode::kConstraintViolation: return "ConstraintViolation";
    case ErrorCode::kExecutionFailed: return "ExecutionFailed";
    case ErrorCode::kExecutionTimeout: return "ExecutionTimeout";
    case ErrorCode::kControllerUnavailable: return "ControllerUnavailable";
    case ErrorCode::kTrajectoryRejected: return "TrajectoryRejected";
    case ErrorCode::kCollisionDetected: return "CollisionDetected";
    case ErrorCode::kPreExecuteCollision: return "PreExecuteCollision";
    case ErrorCode::kSceneUnavailable: return "SceneUnavailable";
    case ErrorCode::kSceneStale: return "SceneStale";
    case ErrorCode::kInvalidStateTransition: return "InvalidStateTransition";
    case ErrorCode::kNotIdle: return "NotIdle";
    case ErrorCode::kAlreadyStopped: return "AlreadyStopped";
    case ErrorCode::kCacheMiss: return "CacheMiss";
    case ErrorCode::kInternalError: return "InternalError";
  }
  return "Unknown";
}

/// Result type for fallible operations.
/// Holds either a value of type T or an ErrorCode.
/// ADR-0002: no exceptions in core; use Result<T> instead.
template <typename T>
class Result {
 public:
  static Result Ok(T value) { return Result(std::move(value)); }
  static Result Err(ErrorCode code) { return Result(code); }

  bool ok() const { return std::holds_alternative<T>(data_); }
  ErrorCode error() const { return std::get<ErrorCode>(data_); }
  const T& value() const { return std::get<T>(data_); }
  T& value() { return std::get<T>(data_); }

 private:
  explicit Result(T value) : data_(std::move(value)) {}
  explicit Result(ErrorCode code) : data_(code) {}

  std::variant<T, ErrorCode> data_;
};

/// Specialization for void results (success/failure with no value).
template <>
class Result<void> {
 public:
  static Result Ok() { return Result(ErrorCode::kSuccess); }
  static Result Err(ErrorCode code) { return Result(code); }

  bool ok() const { return code_ == ErrorCode::kSuccess; }
  ErrorCode error() const { return code_; }

 private:
  explicit Result(ErrorCode code) : code_(code) {}
  ErrorCode code_;
};

}  // namespace manyforge::core
