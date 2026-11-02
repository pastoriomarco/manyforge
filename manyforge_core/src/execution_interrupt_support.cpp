#include <manyforge_core/execution_interrupt_support.hpp>

namespace manyforge::core {

bool validation_cancelled_by_interrupt(const ErrorDTO& error) {
  if (error.code != kErrorValidationCollision) {
    return false;
  }
  const auto it = error.details.find("reason");
  return it != error.details.end() &&
         it->second == "collision_validation_cancelled";
}

bool authoritative_collision_replan_candidate(const ErrorDTO& error) {
  if ((error.code != kErrorValidationCollision &&
       error.code != kErrorContactPolicyViolation) ||
      validation_cancelled_by_interrupt(error)) {
    return false;
  }
  const auto decision_it = error.details.find("collision_result_decision");
  if (decision_it == error.details.end() || decision_it->second != "fail") {
    return false;
  }
  const auto status_it = error.details.find("collision_result_engine_status");
  return status_it != error.details.end() && status_it->second == "ok";
}

ErrorDTO authoritative_collision_validation_config_error(
    std::string reason,
    std::map<std::string, std::string> details,
    std::string phase) {
  std::string message =
      "Authoritative collision validation pipeline is not configured";
  if (reason == "missing_collision_world_snapshot_publisher") {
    message =
        "Authoritative collision validation snapshot publisher is not configured";
  } else if (reason == "missing_collision_query_engines") {
    message =
        "Authoritative collision validation query engines are not configured";
  } else if (reason == "snapshot_publisher_exception") {
    message = "Authoritative collision validation snapshot publisher failed";
  } else if (reason == "policy_publisher_exception") {
    message = "Authoritative collision validation policy publisher failed";
  } else if (reason == "snapshot_unavailable") {
    message = "Authoritative collision validation snapshot unavailable";
  } else if (reason == "missing_collision_policy_snapshot") {
    message = "Authoritative collision validation policy snapshot unavailable";
  }
  details.emplace("reason", std::move(reason));
  details.emplace("phase", std::move(phase));
  return ErrorDTO{
      kErrorInvariantViolation,
      message,
      std::move(details)};
}

const char* interrupt_preemption_result_name(
    InterruptPreemptionResult result) noexcept {
  switch (result) {
    using enum InterruptPreemptionResult;
    case kPending:
      return "pending";
    case kSucceeded:
      return "succeeded";
    case kPartial:
      return "partial";
    case kBackendPreemptFailed:
      return "backend_preempt_failed";
    case kNoneRequired:
      return "none_required";
  }
  return "unknown";
}

}  // namespace manyforge::core
