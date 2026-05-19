#pragma once

#include <cstdint>
#include <optional>
#include <functional>
#include <map>
#include <string>

#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// Kernel event types (spec §6.1)
enum class KernelEventType {
  kStateTransition,  // state machine transition
  kPlanStarted,      // planning phase started
  kPlanCompleted,    // planning phase completed
  kCacheHit,         // trajectory found in cache
  kCacheMiss,        // cache lookup returned empty
  kCacheInvalidated, // cached trajectory invalidated
  kValidationStarted,  // validation phase started
  kValidationPassed,   // validation passed
  kValidationFailed,   // validation failed
  kExecuteStarted,     // execution phase started
  kExecuteCompleted,   // execution phase completed
  kStopRequested,      // stop requested
  kStopIssued,         // stop issued to backend
  kPauseRequested,     // pause requested
  kResumeRequested,    // resume requested
  kCompletedAcknowledged,  // completed terminal outcome acknowledged
  // Streaming-session events (ADR-0016). `session_id` lives in `details`
  // under the key "session_id" for correlation.
  kStreamingSessionStarted,    // streaming session opened on backend
  kStreamingTargetReceived,    // push_streaming_target accepted by kernel
  kStreamingTargetDispatched,  // backend dispatched a goal for a coalesced target
  kStreamingTargetRejected,    // target rejected (IK fail, validation, etc.)
  kStreamingSessionEnded,      // streaming session terminal (soft-stop or hard cancel)
  kErrorRaised,        // error occurred
  kCollisionDetected,  // collision detected during execution
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

  // General-purpose structured key-value data
  std::map<std::string, std::string> details;
};

/// Event callback type. Must be thread-safe.
using EventCallback = std::function<void(const KernelEvent&)>;

/// Subscription handle for unsubscribing.
struct SubscriptionHandle {
  uint64_t id;
};

}  // namespace manyforge::core
