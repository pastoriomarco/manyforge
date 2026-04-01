#pragma once

#include <cstdint>
#include <optional>
#include <functional>

#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// Kernel event types (spec §6.1)
enum class KernelEventType {
  kStateTransition,  // state machine transition
  kPlanStarted,      // planning phase started
  kPlanCompleted,    // planning phase completed
  kCacheHit,         // trajectory found in cache
  kValidationStarted,  // validation phase started
  kValidationPassed,   // validation passed
  kExecuteStarted,     // execution phase started
  kExecuteCompleted,   // execution phase completed
  kStopRequested,      // stop requested
  kStopIssued,         // stop issued to backend
  kPauseRequested,     // pause requested
  kResumeRequested,    // resume requested
  kErrorRaised,        // error occurred
};

/// Kernel event structure (spec §6.1)
struct KernelEvent {
  KernelEventType type;
  uint64_t execution_id;
  int64_t timestamp_ns;  // from IClock

  // For kStateTransition
  ExecutionState from_state;
  ExecutionState to_state;

  // For kErrorRaised
  std::optional<ErrorDTO> error;
};

/// Event callback type. Must be thread-safe.
using EventCallback = std::function<void(const KernelEvent&)>;

/// Subscription handle for unsubscribing.
struct SubscriptionHandle {
  uint64_t id;
};

}  // namespace manyforge::core
