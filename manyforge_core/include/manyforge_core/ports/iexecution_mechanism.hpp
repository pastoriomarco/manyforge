#pragma once

#include <cstdint>
#include <manyforge_core/execution/execution_goal_key.hpp>
#include <manyforge_core/execution/execution_knowledge.hpp>
#include <manyforge_core/execution/motion_step.hpp>
#include <manyforge_core/execution/prepared_motion.hpp>
#include <manyforge_core/execution/prepared_motion_slot.hpp>
#include <manyforge_core/types/participant_stop_stage.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <vector>

namespace manyforge::core {

/// Why a mechanism call produced no effect. Closed and allocation-free so the
/// containment primitives can stay noexcept.
enum class ExecutionMechanismRefusal : std::uint8_t {
  kNone = 0,
  kUnknownKey,
  /// Key or binding identity does not match the mechanism's current binding.
  kStaleKey,
  kSlotKeyMismatch,
  kInvalidInput,
  /// Bounded ordinary handoff full; explicit full behavior, never a drain
  /// loop. The Stop channel is reserved and never reports this.
  kChannelFull,
  kUnsupported,
};

/// Immediate result of one mechanism call. The disposition reuses the
/// Stop-stage attempt enum:
/// - kNotDispatched: refused before any transport effect (known no effect);
/// - kDispatched: submitted to the mechanism (phase `submitted`);
/// - kEffectUncertain: outcome unknown (e.g. lost acknowledgement).
/// An uncertain result never causes an implicit resend or replacement.
struct ExecutionMechanismCallResult final {
  StopStageAttemptDispositionDTO disposition{StopStageAttemptDispositionDTO::kNotDispatched};
  ExecutionMechanismRefusal refusal{ExecutionMechanismRefusal::kNone};
};

/// Local and source timestamps of one observed fact. Local values are this
/// receiver's monotonic clock; source values are never compared with another
/// machine's monotonic clock.
struct ExecutionMechanismFactStamp final {
  std::uint64_t source_sequence{};
  std::int64_t source_timestamp_ns{};
  std::int64_t received_local_ns{};
};

/// Boundary at which an adapter's computed commands count as realized.
/// Mechanism-neutral: native = the actuator loop wrote them; Isaac = the
/// bridge published them unsuppressed; xArm = the supervised session
/// submitted them. A successful return that suppressed or coalesced a command
/// is reported as such and never counts as realized. Realization is not
/// downstream delivery, position reached or rest.
enum class CommandRealizationBoundary : std::uint8_t {
  kLoopInterfaceWrite = 0,
  kTransportPublication,
  kDeviceSessionSubmission,
};

/// Command-realization contract an adapter declares in its own code for its
/// mechanism; completion and "scale applied" use exactly this rule.
struct CommandRealizationContract final {
  CommandRealizationBoundary boundary{CommandRealizationBoundary::kLoopInterfaceWrite};
  /// The mechanism may suppress a write while returning success.
  bool may_suppress{};
  /// The mechanism may replace an unsent command by a newer one; skipped
  /// sequences are reported separately from the submitted one.
  bool may_coalesce{};

  friend constexpr bool operator==(const CommandRealizationContract&,
                                   const CommandRealizationContract&) = default;
};

/// Command-realization facts under the adapter's declared contract.
/// Suppressed and coalesced commands are counted separately and never count
/// as realized. Downstream delivery is reported only where the device
/// provides it and is never inferred from realization.
struct CommandRealizationFacts final {
  std::uint64_t last_computed_sequence{};
  std::uint64_t last_realized_sequence{};
  std::uint64_t suppressed_count{};
  std::uint64_t coalesced_count{};
  /// The command for the end of the path was realized (completion input).
  bool final_command_realized{};
  std::optional<std::uint64_t> last_delivered_sequence;
  ExecutionMechanismFactStamp stamp;
};

/// Downstream fence/drain progress for a key. Retirement requires kFenced or
/// kDrained; until then the outcome stays unresolved and dependent execution
/// on the binding stays closed.
enum class DownstreamFenceState : std::uint8_t {
  kOpen = 0,
  /// Fencing was requested or publication has stopped, but commands already
  /// queued downstream may still take effect. An adapter that can only prove
  /// it stopped publishing reports this state, never `kFenced`.
  kFenceRequested,
  /// No command of the key, new or already queued downstream, can take effect
  /// any more: publication has stopped and the downstream boundary rejects or
  /// has discarded anything still in flight.
  kFenced,
  /// Every previously emitted command was consumed or discarded downstream.
  kDrained,
  kOutcomeUnknown,
};

/// Per-key mechanism status. Phase and effect are separate facts for each
/// command kind. Retirement's two facts are separate fields: the loop no
/// longer references the slot (`slot_referenced == false`, slot may be
/// reclaimed) and downstream commands are fenced or drained.
struct ExecutionMechanismKeyStatus final {
  ExecutionGoalKey key;
  ControllerGoalGeneration last_consumed_goal_generation{0U};
  RuntimeScaleGeneration last_consumed_scale_generation{0U};
  StopGeneration last_consumed_stop_generation{0U};
  ExecutionCommandKnowledge dispatch;
  ExecutionCommandKnowledge scale;
  ExecutionCommandKnowledge stop;
  MotionStepStatus last_step_status{MotionStepStatus::kRunning};
  CommandRealizationFacts realization;
  bool slot_referenced{};
  DownstreamFenceState downstream{DownstreamFenceState::kOpen};
  ExecutionMechanismFactStamp stamp;
};

/// Two independent liveness checks, both on local monotonic time and both
/// using the retained containment primitives, including at scale zero.
/// Neither repairs lifecycle or resumes motion.
struct ExecutionLoopHealth final {
  /// Detected by the running loop.
  bool host_lease_valid{};
  std::int64_t host_lease_expiry_local_ns{};
  std::uint64_t loop_tick_count{};
  std::int64_t last_tick_local_ns{};
  /// Detected by a watchdog outside the update thread.
  bool stall_watchdog_tripped{};
  std::int64_t stall_watchdog_checked_local_ns{};
  ExecutionMechanismFactStamp stamp;
};

struct ExecutionMechanismStatus final {
  RuntimeEpoch source_epoch{std::string{}};
  ExecutionLoopHealth loop;
  std::vector<ExecutionMechanismKeyStatus> keys;
};

/// Mechanism port: the NRT side of one adapter-owned update loop. Replaces
/// the mechanism half of `IExecutionBackend`; contains no Prepare/Dispatch/
/// Stop policy, journal or recovery state machine.
///
/// Every mutating call is noexcept and returns an explicit disposition.
/// Ordinary handoffs are bounded with a fixed maximum per tick and explicit
/// full behavior. Stop uses a reserved channel consumed first in the tick and
/// never queues behind ordinary traffic; no mutex shared with transport waits
/// spans PostStop/Hold/Cancel/Fence, so Stop stays serviceable while a
/// submission is in flight. No call moves anything on retirement.
class IExecutionMechanism {
 public:
  virtual ~IExecutionMechanism() = default;

  /// Hand a stable prepared slot to the loop. `slot.motion.key` must equal
  /// `key` and `slot.sealed.participant_digest` its
  /// `prepared_contract_digest` (else kSlotKeyMismatch). The caller keeps the
  /// slot alive and unmodified until status reports it unreferenced. The loop
  /// reads only `slot.motion`; a transport adapter transmits only
  /// `slot.sealed` (outside RT) and the receiver rebuilds the motion with the
  /// shared Core builder.
  [[nodiscard]] virtual ExecutionMechanismCallResult Submit(
      const ExecutionGoalKey& key, const PreparedMotionSlot& slot) noexcept = 0;
  /// Install a replacement under `next_key` (a newer goal generation) in
  /// place of `key`, through `InstallReplacementMotionStep` at the loop's
  /// installation tick: Stop and trip cursor preserved, continuity checked
  /// against the actual emitted command, slider never applied twice. A
  /// refused installation leaves `key` authoritative with its authorized
  /// braking. Same slot rules and lifetime as `Submit`.
  [[nodiscard]] virtual ExecutionMechanismCallResult Replace(
      const ExecutionGoalKey& key, const ExecutionGoalKey& next_key,
      const PreparedMotionSlot& slot) noexcept = 0;
  /// Ask the loop to stop referencing the key's slot (retirement fact one).
  [[nodiscard]] virtual ExecutionMechanismCallResult Release(const ExecutionGoalKey& key) noexcept = 0;

  [[nodiscard]] virtual ExecutionMechanismCallResult PostScale(
      const ExecutionGoalKey& key, const ScaleTarget& target) noexcept = 0;
  /// Reserved Stop channel; consumed before any ordinary input in the tick.
  [[nodiscard]] virtual ExecutionMechanismCallResult PostStop(
      const ExecutionGoalKey& key, const StopTransition& transition) noexcept = 0;

  /// Device primitives. A cancel acknowledgement never proves stationary.
  [[nodiscard]] virtual ExecutionMechanismCallResult Hold(const ExecutionGoalKey& key) noexcept = 0;
  [[nodiscard]] virtual ExecutionMechanismCallResult Cancel(const ExecutionGoalKey& key) noexcept = 0;
  /// Fence downstream commands of the key. Retirement fact two holds only in
  /// `kFenced` or `kDrained`; until then the outcome stays unresolved and
  /// dependent execution on the binding stays closed.
  [[nodiscard]] virtual ExecutionMechanismCallResult Fence(const ExecutionGoalKey& key) noexcept = 0;

  /// Renew the execution host's lease on the loop. Issued by the participant
  /// execution in the host process itself, never by Manager liveness on its
  /// behalf. The loop derives the expiry from its own monotonic receipt time
  /// plus `lease_duration_ns`; no absolute time crosses machines. On expiry
  /// the loop applies the containment primitive already authorized for the
  /// binding, including at scale zero. Renewal never resumes motion or clears
  /// containment.
  [[nodiscard]] virtual ExecutionMechanismCallResult RenewHostLease(
      std::int64_t lease_duration_ns) noexcept = 0;

  /// Observation only: never repeats effects, clears faults or resumes.
  [[nodiscard]] virtual ExecutionMechanismStatus ReadMechanismStatus() const = 0;
};

}  // namespace manyforge::core
