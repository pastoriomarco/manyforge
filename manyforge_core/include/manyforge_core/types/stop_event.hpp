#pragma once

// Stop state model (2026-11-13): the small value types every layer shares.
// The Kernel's own state structures (participant stop state, hold state,
// event ring) are in `manyforge_core/stop_state.hpp`; this header carries
// only what other headers (operation state, recovery, rule outcomes, engine
// ports) and the wire codecs name. Wire tokens are written once here
// (`to_string`); the contract file lists the same tokens for Python/TS.

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/strong_id.hpp>
#include <string_view>

namespace manyforge::core {

/// Payload of the journal rows `stop_event_accepted` / `stop_event_completed`.
inline constexpr std::string_view kStopEventPayloadSchema = "manyforge.stop_event.v1";
/// `read_stop` reply.
inline constexpr std::string_view kExecutionStopStatusSchema = "manyforge.execution_stop_status.v2";
/// Journal row kinds that replace `stop_acceptance_projected` and
/// `stop_terminal_projected`.
inline constexpr std::string_view kStopEventAcceptedRowKind = "stop_event_accepted";
inline constexpr std::string_view kStopEventCompletedRowKind = "stop_event_completed";
/// Source label of a Halt raised by a failed Stop (§3.6 F1).
inline constexpr std::string_view kStopFailedSourceLabel = "kernel.stop_failed";

/// Stage index meaning "no stage" (natural finish, pre-dispatch cancel, or
/// no stage contained the participant).
inline constexpr std::uint8_t kNoStopStage = 0xFFU;

/// Participant stop phase (§2.1). Within one attempt it only rises.
enum class StopPhase : std::uint8_t {
  kRunning = 0,
  kStopping,
  kStopped,
  kFailed,
};

[[nodiscard]] constexpr std::string_view to_string(StopPhase value) noexcept {
  switch (value) {
    case StopPhase::kRunning:
      return "running";
    case StopPhase::kStopping:
      return "stopping";
    case StopPhase::kStopped:
      return "stopped";
    case StopPhase::kFailed:
      return "failed";
  }
  return {};
}

/// The physical stop plan of one attempt, fixed when the attempt starts.
enum class StopMode : std::uint8_t {
  kOrdinary = 0,
  kFault,
  kInterrupt,
  kHard,
  kSpringReturn,
};

[[nodiscard]] constexpr std::string_view to_string(StopMode value) noexcept {
  switch (value) {
    case StopMode::kOrdinary:
      return "ordinary";
    case StopMode::kFault:
      return "fault";
    case StopMode::kInterrupt:
      return "interrupt";
    case StopMode::kHard:
      return "hard";
    case StopMode::kSpringReturn:
      return "spring_return";
  }
  return {};
}

/// Attribution class, ordered: a later Stop raises it lexicographically on
/// (class, rule priority).
enum class StopAttributionClass : std::uint8_t {
  kProgramOwn = 0,
  kRule,
  kOperator,
};

[[nodiscard]] constexpr std::string_view to_string(StopAttributionClass value) noexcept {
  switch (value) {
    case StopAttributionClass::kProgramOwn:
      return "program_own";
    case StopAttributionClass::kRule:
      return "rule";
    case StopAttributionClass::kOperator:
      return "operator";
  }
  return {};
}

/// Requested kind and applied level of one Stop event (§2.4). `applied`
/// differs from `requested` only when the invalid-Stop rule widened it, and
/// is then `kStopAll` or `kHalt` (PD-1).
enum class StopKind : std::uint8_t {
  kGroups = 0,
  kStopAll,
  kHalt,
  kOperation,
  kCellStop,
  kRuleFailNode,
  kRuleHold,
  kInterruptRule,
  kRunnerLoss,
  kProgramEnd,
  kInternalFault,
  kTakeover,
  kPartialDispatch,
  kRuntimeCollision,
  kHoldTimeout,
  kOperationReset,
  kPreparedDispatchInvalid,
  kPlannedStop,
  kStopFailedHalt,
};

[[nodiscard]] constexpr std::string_view to_string(StopKind value) noexcept {
  switch (value) {
    case StopKind::kGroups:
      return "groups";
    case StopKind::kStopAll:
      return "stop_all";
    case StopKind::kHalt:
      return "halt";
    case StopKind::kOperation:
      return "operation";
    case StopKind::kCellStop:
      return "cell_stop";
    case StopKind::kRuleFailNode:
      return "rule_fail_node";
    case StopKind::kRuleHold:
      return "rule_hold";
    case StopKind::kInterruptRule:
      return "interrupt_rule";
    case StopKind::kRunnerLoss:
      return "runner_loss";
    case StopKind::kProgramEnd:
      return "program_end";
    case StopKind::kInternalFault:
      return "internal_fault";
    case StopKind::kTakeover:
      return "takeover";
    case StopKind::kPartialDispatch:
      return "partial_dispatch";
    case StopKind::kRuntimeCollision:
      return "runtime_collision";
    case StopKind::kHoldTimeout:
      return "hold_timeout";
    case StopKind::kOperationReset:
      return "operation_reset";
    case StopKind::kPreparedDispatchInvalid:
      return "prepared_dispatch_invalid";
    case StopKind::kPlannedStop:
      return "planned_stop";
    case StopKind::kStopFailedHalt:
      return "stop_failed_halt";
  }
  return {};
}

/// Why the invalid-Stop rule widened a request (PD-1).
enum class StopWidenReason : std::uint8_t {
  kNone = 0,
  kUnknownTarget,
  kMalformed,
  kKeyConflict,
  kStaleStart,
  kUnreadableKind,
};

[[nodiscard]] constexpr std::string_view to_string(StopWidenReason value) noexcept {
  switch (value) {
    case StopWidenReason::kNone:
      return "none";
    case StopWidenReason::kUnknownTarget:
      return "unknown_target";
    case StopWidenReason::kMalformed:
      return "malformed";
    case StopWidenReason::kKeyConflict:
      return "key_conflict";
    case StopWidenReason::kStaleStart:
      return "stale_start";
    case StopWidenReason::kUnreadableKind:
      return "unreadable_kind";
  }
  return {};
}

/// Result of one Stop event, set at completion (§2.4, §4.2).
enum class StopEventResult : std::uint8_t {
  kPending = 0,
  kNoEffect,
  kStopped,
  kStoppedEscalated,
  kSomeFailed,
};

[[nodiscard]] constexpr std::string_view to_string(StopEventResult value) noexcept {
  switch (value) {
    case StopEventResult::kPending:
      return "pending";
    case StopEventResult::kNoEffect:
      return "no_effect";
    case StopEventResult::kStopped:
      return "stopped";
    case StopEventResult::kStoppedEscalated:
      return "stopped_escalated";
    case StopEventResult::kSomeFailed:
      return "some_failed";
  }
  return {};
}

/// Which hold fields one event raised (bit set in `StopEventRecord::hold_change`).
enum class StopHoldChange : std::uint8_t {
  kNodes = 1U << 0U,
  kAllGroups = 1U << 1U,
  kFrozen = 1U << 2U,
  kRule = 1U << 3U,
};

/// The run's frozen level (§2.3), raised monotonically.
enum class RunFrozenLevel : std::uint8_t {
  kNone = 0,
  kHalt,
  kRunnerLoss,
  kProgramEnd,
};

[[nodiscard]] constexpr std::string_view to_string(RunFrozenLevel value) noexcept {
  switch (value) {
    case RunFrozenLevel::kNone:
      return "none";
    case RunFrozenLevel::kHalt:
      return "halt";
    case RunFrozenLevel::kRunnerLoss:
      return "runner_loss";
    case RunFrozenLevel::kProgramEnd:
      return "program_end";
  }
  return {};
}

/// One participant's stop outcome as other owners read it: the engine's
/// cold spring-return preparation (K11) and the takeover readers. Plain data
/// copied from the participant's stop state under the Kernel lock.
struct ParticipantStopOutcomeDTO final {
  /// The event that started the attempt this outcome belongs to.
  StopEventId stop_event_id{0U};
  StopPhase phase{StopPhase::kRunning};
  StopMode mode{StopMode::kOrdinary};
  /// Index into the executed plan's stages; `kNoStopStage` when none.
  std::uint8_t containing_stage{kNoStopStage};
  bool escalated{};
  std::uint16_t attempt{};

  friend bool operator==(const ParticipantStopOutcomeDTO&,
                         const ParticipantStopOutcomeDTO&) = default;
};

}  // namespace manyforge::core
