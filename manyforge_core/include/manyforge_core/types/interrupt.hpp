#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/execution_status.hpp>

namespace manyforge::core {

enum class InterruptPreemptionAction {
  kCancelPlanning,
  kCancelValidation,
  kPreemptTrajectory,
  /// Move-scoped non-fault completion. Reserved for
  /// `scope_kind == kMove` node policies. Stops
  /// trajectory execution on a desired signal trip and produces a
  /// `MotionExecutionOutcomeDTO` with `terminal_cause = signal_trip`
  /// when (and only when) the backend confirms stop and a valid
  /// `ExecutionStopSnapshotDTO` is captured.
  kCompleteMoveOnSignal,
};

/// Node-rule (move-scoped) dispositions only; global rules have none.
enum class InterruptPostLatchDisposition {
  /// Latch is cleared only after the matching `MotionExecutionOutcomeDTO`
  /// has been durably emitted to the operational event stream and
  /// stored in the kernel's terminal-outcome surface. Only valid for
  /// `scope_kind=move` policies whose `preemption_action ==
  /// kCompleteMoveOnSignal`.
  kAutoClearAfterMotionOutcome,
  kFaultTerminal,
};

/// Activation modes for move-scoped policy bindings.
enum class MotionInterruptActivationMode {
  /// Default for touch probes: kernel samples the source at arm time;
  /// if active or unknown, the move is rejected before dispatch.
  kRequireInactiveAtArm,
  /// Records baseline at arm time; only a post-arm transition latches.
  kFreshEdgeAfterArm,
  /// Latches immediately when already active; must be explicitly allowed
  /// in the policy's `allowed_activation_modes`.
  kTripIfAlreadyActive,
  /// Reserved for surface following and compliant maintenance; v1
  /// adapters return `diagnostic_incomplete` if they cannot support it.
  kMaintainLevelWithHysteresis,
};

/// Controlled-stop mode requested by a move-scoped interrupt scope
/// (`MotionInterruptStopProfileDTO`).
///
/// `kSpringBackHold` is the non-critical path-preserving return profile:
/// forward decelerate on the accepted path, then return on the same path to the
/// trip cursor. `kSoftOnPath` is a path-preserving ramp along the accepted
/// trajectory.
/// `kHardDeceleration` is an acceleration-limited stop that may leave
/// the path. None of these is a safety e-stop; e-stop / protective
/// stops remain higher-priority session/fault policies and produce
/// `interrupted_fault` or `failed`, never `signal_trip`.
enum class MotionInterruptStopMode {
  kSpringBackHold,
  kSoftOnPath,
  kHardDeceleration,
  /// The active trajectory's passed points plus one zero-velocity point
  /// at the trip position, sent at once with no planning or checks.
  kSpringBackFast,
};

/// Which backend/controller surface actually issued the controlled
/// stop (`MotionTripOutcomeDTO.stop_command_source`).
/// `kDiagnosticOther` is reserved for backends whose stop path does
/// not match a named taxonomy and MUST carry a stable reason in the
/// outcome's `diagnostics`.
enum class MotionInterruptStopCommandSource {
  kFjtPreemptHold,
  kFjtPreemptPathRamp,
  kJtcSpeedScalingRamp,
  kJtcCancelDecelerateOnCancel,
  kJtcCancelUnprofiledHold,
  kRobotNativeStop,
  kDiagnosticOther,
};

/// Motion-node category metadata carried on a `MotionInterruptScopeDTO`.
/// The kernel uses the category to enforce default fallback rules at
/// arm time (conjunctive fallback rule; node-category defaults).
enum class MotionInterruptNodeCategory {
  /// `MoveUntilSignal` and other generic search nodes. May accept any
  /// fallback the backend capability profile and policy allow.
  kGeneric,
  /// `ProbeSearchMove` and other precision-contact node families.
  /// Defaults to no fallback — the precision contract cannot tolerate
  /// silent substitution of a different stop mode at trip time.
  kPrecisionContact,
};

/// Whether a policy is always active (`kSession`) or only when armed by
/// a `MotionInterruptScopeDTO` on the active `ExecuteRequestDTO`
/// (`kMove`).
enum class InterruptPolicyScopeKind {
  kSession,
  kMove,
};

enum class InterruptFaultKind {
  kStale,
  kDisconnected,
  kIngestOverflow,
  kSourceClassViolation,
};

enum class InterruptPreemptionResult {
  kPending,
  kSucceeded,
  kPartial,
  kBackendPreemptFailed,
  kNoneRequired,
};

inline constexpr std::size_t kMaxInterruptStructuredValueEntries = 64U;
inline constexpr std::size_t kMaxInterruptStructuredValueKeyBytes = 64U;
inline constexpr std::size_t kMaxInterruptStructuredValueStringBytes = 256U;

/// One value in the closed, flat interrupt diagnostic/adapter object.
///
/// Callers MUST use the named factories below for string literals. Assigning a
/// `const char*` directly to `std::variant<bool, double, std::string>` selects
/// the boolean alternative under C++ overload resolution.
using InterruptStructuredScalar = std::variant<bool, double, std::string>;
using InterruptStructuredValue =
    std::map<std::string, InterruptStructuredScalar>;

[[nodiscard]] inline InterruptStructuredScalar interrupt_structured_bool(
    bool value) {
  return InterruptStructuredScalar(std::in_place_type<bool>, value);
}

[[nodiscard]] inline InterruptStructuredScalar interrupt_structured_number(
    double value) {
  return InterruptStructuredScalar(std::in_place_type<double>, value);
}

[[nodiscard]] inline InterruptStructuredScalar interrupt_structured_string(
    std::string_view value) {
  return InterruptStructuredScalar(std::in_place_type<std::string>, value);
}

/// Explicit projection into legacy string-only event/error detail surfaces.
/// This is presentation/audit conversion only; it MUST NOT be used to recover
/// policy, identity, timing, or command semantics from structured values.
[[nodiscard]] inline std::string interrupt_structured_scalar_text(
    const InterruptStructuredScalar& value) {
  if (const auto* boolean = std::get_if<bool>(&value)) {
    return *boolean ? "true" : "false";
  }
  if (const auto* number = std::get_if<double>(&value)) {
    return std::to_string(*number);
  }
  return std::get<std::string>(value);
}

[[nodiscard]] inline std::map<std::string, std::string>
project_interrupt_structured_value_to_text(
    const InterruptStructuredValue& value) {
  std::map<std::string, std::string> projected;
  for (const auto& [key, scalar] : value) {
    projected.emplace(key, interrupt_structured_scalar_text(scalar));
  }
  return projected;
}

/// Returns a stable reason token when a closed structured value violates its
/// portable key, scalar, or collection bounds. `require_nonempty` is used only
/// by fields whose contract assigns meaning to the presence of an object (for
/// example structured level-source hysteresis).
[[nodiscard]] std::optional<std::string>
interrupt_structured_value_error(const InterruptStructuredValue& value,
                                 bool require_nonempty = false);

using InterruptSignalValue =
    std::variant<bool, double, InterruptStructuredValue>;

struct InterruptSignalUpdate {
  std::string source_id;
  std::int64_t sequence_id = 0;
  std::int64_t timestamp_ns = 0;
  InterruptSignalValue value = false;
  std::optional<std::int64_t> freshness_ns;
  std::optional<double> confidence;
  std::optional<InterruptStructuredValue> covariance;
  std::optional<InterruptFaultKind> fault_kind;
  InterruptStructuredValue metadata;
};

}  // namespace manyforge::core
