#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

// ---------------------------------------------------------------------------
// Execution port (shared execution design §3).
//
// ROS-free, transport-independent contract between Core and the mechanism that
// executes one robot participant's trajectories (today a stock JTC through the
// ManyForge JTC adapter; later possibly a direct vendor SDK). It asks only for
// facts such a mechanism can supply and never manufactures facts it does not
// report. Stop, recovery, homing and timeout policy stay in Core.
//
// Binding: one port instance serves one binding epoch. On rebinding
// (controller reactivated, hardware replaced, lane re-bound) the host
// constructs a new instance; there is no in-place rebind.
// ---------------------------------------------------------------------------

/// Increments whenever the controller/hardware binding changes (controller
/// reactivated, hardware replaced, lane re-bound). Every observation carries
/// it; facts from different epochs are never combined.
struct ExecutionBindingEpochTag final {};
using ExecutionBindingEpoch = StrongCounter<ExecutionBindingEpochTag>;

/// The mechanism's own goal identity (FollowJointTrajectory goal UUID on JTC),
/// recorded by the adapter with the goal key at every send. Opaque to Core:
/// used for traceability only, never parsed or compared across mechanisms.
/// May be empty while the goal is pending or after it was rejected (the
/// mechanism has not assigned or reported one).
struct MechanismGoalIdTag final {};
using MechanismGoalId = StrongString<MechanismGoalIdTag>;

/// The adapter's local receipt counter for one sample stream. Bookkeeping
/// only: it cannot reveal messages lost before receipt (`controller_state`
/// carries no producer sequence).
struct ExecutionReceiptCountTag final {};
using ExecutionReceiptCount = StrongCounter<ExecutionReceiptCountTag>;

/// Core's per-goal identity on the port.
///
/// Reuses the Kernel-minted pair already carried by dispatch bindings,
/// participant state and Stop stages (`ResolvedExecutionBindingDTO`,
/// `OperationParticipantStateDTO`): `controller_goal_id` names the operation
/// participant's goal and `controller_goal_generation` advances on every
/// replacement of it (Stop ramp, spring-back), so `Replace` always names two
/// distinct keys. Binding identity is deliberately not part of the key: every
/// observation carries the binding epoch instead (§3.1).
struct ExecutionGoalKey final {
  ControllerGoalId controller_goal_id{std::string{}};
  ControllerGoalGeneration controller_goal_generation{0U};

  friend bool operator==(const ExecutionGoalKey&, const ExecutionGoalKey&) = default;
  friend auto operator<=>(const ExecutionGoalKey&, const ExecutionGoalKey&) = default;
};

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

/// What a command did. `kDispatched` means sent to the mechanism, not
/// accepted or applied: acceptance and effect are observed through
/// `ReadGoal()` / `ReadSample()`. `kEffectUncertain` means the send may or
/// may not have reached the mechanism; Core must treat it as possibly sent.
enum class ExecutionPortDispatch {
  kNotDispatched,
  kDispatched,
  kEffectUncertain,
};

/// Why a command was not dispatched. Present only with `kNotDispatched`.
enum class ExecutionPortRefusal {
  /// The named key is not the mechanism's current goal (or is already used).
  kStaleKey,
  /// The input violates the command's contract (shape, timing, range).
  kInvalidInput,
  /// The lane does not support the command (see `ExecutionLaneFactsDTO`).
  kUnsupported,
  /// The controller or hardware is not active/bound.
  kNotReady,
};

struct ExecutionPortCommandResultDTO final {
  ExecutionPortDispatch dispatch{ExecutionPortDispatch::kNotDispatched};
  std::optional<ExecutionPortRefusal> refusal{};
  /// Diagnostic detail for a refusal or an uncertain effect.
  std::optional<ErrorDTO> error{};

  friend bool operator==(const ExecutionPortCommandResultDTO&,
                         const ExecutionPortCommandResultDTO&) = default;
};

/// One replacement's start on the SAME controller clock as its associated
/// source sample. The shifted trajectory's time zero and this stamp are one
/// command; this is neither a local-clock timestamp nor installation proof.
/// EXPERIMENTAL release-timer outcome of one replacement (Core holds before
/// the send through its injected waiter; the port never sleeps).
enum class ExecutionReleaseHold {
  kNone,      // no release timer
  kHeld,      // held until the release instant, then the existing checks ran
  kMissed,    // the instant had already passed: sent at once (timer-free path)
  kDeadline,  // holding would cross an existing deadline: sent at once
};

/// EXPERIMENTAL producer-policy record of one scheduled replacement, present
/// only when a lane experiment flag is set (diagnostics and the release
/// instant). Controller-clock stamps except `release_local_ns`, which is on
/// the local clock of the sample receipts.
struct ExecutionReplacementExperimentDTO final {
  /// Controller update stamp (`controller_state` header) the grid comes from.
  TimePoint source_stamp_ns{0};
  /// Target tick T: the existing whole-period start; the stamp is T, or
  /// T + P/2 with the mid-tick flag.
  TimePoint target_tick_ns{0};
  /// Release timer only: send at T - P/2 - expected latency (receipt-mapped).
  std::optional<TimePoint> release_local_ns{};
  ExecutionReleaseHold release_hold{ExecutionReleaseHold::kNone};
  Duration expected_latency_ns{0};
  Duration start_allowance_ns{0};
  double factor{0.0};
  /// Flags applied to this replacement.
  bool midtick_stamp{false};
  bool v0_payload{false};
  bool spring_back_fast{false};
  bool start_allowance_override{false};

  friend bool operator==(const ExecutionReplacementExperimentDTO&,
                         const ExecutionReplacementExperimentDTO&) = default;
};

struct ExecutionReplacementStartDTO final {
  ExecutionBindingEpoch binding_epoch{0U};
  TimePoint controller_start_stamp_ns{0};
  /// Present only with a lane experiment flag (diagnostics; the port logs it
  /// and sends exactly as without it).
  std::optional<ExecutionReplacementExperimentDTO> experiment{};

  friend bool operator==(const ExecutionReplacementStartDTO&,
                         const ExecutionReplacementStartDTO&) = default;
};

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

enum class ExecutionGoalPhase {
  /// Sent, acceptance not yet reported.
  kPending,
  /// Accepted by the mechanism, not yet observed executing.
  kAccepted,
  kExecuting,
  kTerminal,
};

enum class ExecutionGoalResult {
  kSucceeded,
  kAbortedPathTolerance,
  kAbortedGoalTolerance,
  kCanceled,
  kRejected,
  /// Replaced by `ExecutionGoalObservationDTO::preempted_by`. Reported only
  /// when that successor was accepted on the same binding epoch while the
  /// controller stayed active; otherwise the result is `kEndedUnknown`.
  kPreemptedBy,
  /// The goal ended without a reported cause (e.g. controller deactivated,
  /// binding changed); the controller's lifecycle facts decide.
  kEndedUnknown,
};

[[nodiscard]] inline const char* to_string(ExecutionGoalResult result) noexcept {
  switch (result) {
    case ExecutionGoalResult::kSucceeded:
      return "succeeded";
    case ExecutionGoalResult::kAbortedPathTolerance:
      return "aborted_path_tolerance";
    case ExecutionGoalResult::kAbortedGoalTolerance:
      return "aborted_goal_tolerance";
    case ExecutionGoalResult::kCanceled:
      return "canceled";
    case ExecutionGoalResult::kRejected:
      return "rejected";
    case ExecutionGoalResult::kPreemptedBy:
      return "preempted_by";
    case ExecutionGoalResult::kEndedUnknown:
      return "ended_unknown";
  }
  return "unknown";
}

/// One action-feedback message of one goal (FollowJointTrajectory feedback
/// on JTC). Unlike `ExecutionControllerSampleDTO` it is attributed to its goal
/// by the mechanism itself. Every joint vector follows
/// `ReadLaneFacts().joint_names`.
struct ExecutionGoalFeedbackDTO final {
  /// Mechanism clock; never compared with local stamps.
  TimePoint source_stamp_ns{0};
  /// Local monotonic receipt time.
  TimePoint local_receipt_stamp_ns{0};
  /// Path time since the start of this goal's trajectory.
  double cursor_s{0.0};
  /// Desired state of this goal's trajectory at the cursor.
  std::vector<double> desired_positions_rad;
  std::vector<double> desired_velocities_rad_s;
  std::vector<double> desired_accelerations_rad_s2;
  /// Actual positions as reported with the feedback.
  std::vector<double> actual_positions_rad;

  friend bool operator==(const ExecutionGoalFeedbackDTO&,
                         const ExecutionGoalFeedbackDTO&) = default;
};

/// Goal records each adapter keeps (§3.1): two, the active goal and its
/// predecessor (so `kPreemptedBy` stays readable during a Stop replacement).
/// It bounds the facts kept, not the number of motions. When a new goal
/// arrives the oldest finished record is dropped; if both records are still
/// unfinished the new goal is refused with `kNotReady` (never a silent drop). Ownership:
/// Core is the authority against replay across goals and operations (it mints
/// every controller goal id uniquely and never reuses one); the mechanism
/// accepts commands only for its current active or pending key and refuses any
/// other key as stale, without history and without ordering opaque ids. A
/// dropped record reads as `nullopt`; Core reads a goal's terminal result
/// before submitting the next goal.
inline constexpr std::size_t kExecutionGoalRecordCapacity = 2;

/// Goal-scoped facts for one key.
struct ExecutionGoalObservationDTO final {
  ExecutionGoalKey key;
  MechanismGoalId mechanism_goal_id{std::string{}};
  /// Binding epoch the goal was sent on.
  ExecutionBindingEpoch binding_epoch{0U};
  ExecutionGoalPhase phase{ExecutionGoalPhase::kPending};
  /// Set only in `kTerminal`.
  std::optional<ExecutionGoalResult> result{};
  /// Set only with `ExecutionGoalResult::kPreemptedBy`.
  std::optional<ExecutionGoalKey> preempted_by{};
  /// Local monotonic time at which the acceptance was received.
  std::optional<TimePoint> accepted_stamp_ns{};
  /// Latest action feedback of this goal; absent before the first one.
  std::optional<ExecutionGoalFeedbackDTO> latest_feedback{};
  /// Local monotonic receipt time of the terminal result (every result,
  /// including `kCanceled`). Set only in `kTerminal`.
  std::optional<TimePoint> terminal_local_receipt_stamp{};
  /// Local monotonic time at which the cancel was sent to the mechanism
  /// (absent while a cancel on a pending goal is only latched).
  std::optional<TimePoint> cancel_sent_local_stamp{};
  /// Local monotonic receipt time of the mechanism's POSITIVE acknowledgement
  /// of this exact cancel (for JTC: an accepted cancel response for this
  /// goal's UUID). A negative or ambiguous cancel response never sets it; the
  /// cancel then stays uncertain and Core's Stop deadline decides. An
  /// acknowledgement is neither the terminal result nor the hard-stop finish.
  std::optional<TimePoint> cancel_ack_local_receipt_stamp{};

  friend bool operator==(const ExecutionGoalObservationDTO&,
                         const ExecutionGoalObservationDTO&) = default;
};

/// One controller-wide sample (JTC `controller_state`).
///
/// It is not assigned to any goal. Core associates it with a goal only by
/// evidence (the goal's own action feedback, or the observed switch of the
/// cursor to the new trajectory); a stamp after acceptance is not sufficient
/// (§3.1). Every joint vector follows `ReadLaneFacts().joint_names` (the
/// binding's controller joint order), independent of any submitted trajectory.
///
/// Cancel association (Core's hard-stop finish, Q5, Q35): a sample counts for
/// a cancel only if its local receipt is ordered after the cancel was sent
/// (`cancel_sent_local_stamp`) AND it shows the controller actually switching
/// to its cancel trajectory (cursor restarting, or the reference jumping to
/// the cancel trajectory's start). The switch may precede the receipt of the
/// cancel acknowledgement or result. An old zero sample, or a trajectory frozen
/// at factor 0, never counts. Q35: the hard stop is finished when the cancel's
/// associated braking reference reaches zero velocity (Q5), or when the
/// controller reports an applied factor of 0 and has positively acknowledged
/// this cancel (its cancel trajectory cannot advance at factor 0; installing it
/// may still step the commanded position once to the cancel trajectory's start,
/// e.g. the measured position). Neither is physical-rest or loop-consumption
/// evidence; otherwise Core's Stop deadline decides. Local receipt ordering and the same
/// controller's source stamps may be used together; controller source stamps
/// are never compared with local clocks. The exact rule is fixed by the
/// characterization of concrete callback orders.
struct ExecutionControllerSampleDTO final {
  /// Controller clock. Orders samples of one controller only; never compared
  /// with local stamps or with another machine's clock.
  TimePoint source_stamp_ns{0};
  /// Local monotonic receipt time.
  TimePoint local_receipt_stamp_ns{0};
  ExecutionReceiptCount local_receipt_count{0U};
  ExecutionBindingEpoch binding_epoch{0U};
  /// Path time since the start of the currently sampled trajectory. Restarts
  /// at every replacement and at hold; Core maps it back to the original path.
  double cursor_s{0.0};
  /// Speed factor as reported applied by the controller; absent when the
  /// controller does not report one. Published is not applied.
  std::optional<double> applied_factor{};
  /// Commanded reference; derivatives in path-time units at the applied factor.
  std::vector<double> reference_positions_rad;
  std::vector<double> reference_velocities_rad_s;
  std::vector<double> reference_accelerations_rad_s2;
  /// Last written command (`output`). Velocities are absent on position-only
  /// lanes.
  std::vector<double> command_positions_rad;
  std::optional<std::vector<double>> command_velocities_rad_s{};
  /// Hardware state. Velocities only where the hardware exports them.
  std::vector<double> measured_positions_rad;
  std::optional<std::vector<double>> measured_velocities_rad_s{};
  /// Position error as reported by the controller.
  std::vector<double> error_positions_rad;

  friend bool operator==(const ExecutionControllerSampleDTO&,
                         const ExecutionControllerSampleDTO&) = default;
};

struct ExecutionReadinessDTO final {
  bool controller_active{false};
  bool controller_bound{false};
  bool hardware_active{false};
  ExecutionBindingEpoch binding_epoch{0U};

  friend bool operator==(const ExecutionReadinessDTO&,
                         const ExecutionReadinessDTO&) = default;
};

/// Controller timing since the previous `ReadTiming()` (Q4). Best-effort:
/// every field is absent when the mechanism did not report it. Telemetry loss
/// (stamp gaps) is distinct from controller lateness (periodicity).
struct ExecutionTimingDTO final {
  ExecutionBindingEpoch binding_epoch{0U};
  /// Controller-manager periodicity statistics.
  std::optional<double> period_mean_s{};
  std::optional<double> period_min_s{};
  std::optional<double> period_max_s{};
  std::optional<double> period_stddev_s{};
  /// Largest gap between consecutive `controller_state` source stamps.
  std::optional<double> max_state_stamp_gap_s{};
  /// Samples received in the window (local receipt counter delta).
  std::optional<std::uint64_t> received_samples{};

  friend bool operator==(const ExecutionTimingDTO&, const ExecutionTimingDTO&) = default;
};

/// How `Cancel` brings the mechanism to rest (Q2: JTC
/// `decelerate_on_cancel` on every lane is `kDecelerate`).
enum class ExecutionCancelSemantics {
  kDecelerate,
  kHold,
};

/// Whether the lane's controller keeps goal tolerances (Q3: real lanes keep
/// them with `goal_time > 0`; simulation lanes erase them).
enum class ExecutionGoalTolerancePolicy {
  kErased,
  kKept,
};

/// EXPERIMENTAL independent opt-ins for scheduled replacements (stock JTC 6.10
/// A/B tests), all default off; set only from the Mock lane's environment.
struct ExecutionReplacementExperimentFlagsDTO final {
  /// `MANYFORGE_EXPERIMENTAL_RELEASE_TIMER`: hold until T - P/2 - latency.
  bool release_timer{false};
  /// `MANYFORGE_EXPERIMENTAL_MIDTICK_STAMP`: stamp T + P/2, D for that stamp.
  bool midtick_stamp{false};
  /// `MANYFORGE_EXPERIMENTAL_V0_PAYLOAD`: no negative-time prefix; the first
  /// point at 0 holds the planned state at the stamp.
  bool v0_payload{false};
  /// `MANYFORGE_EXPERIMENTAL_SCHEDULED_SPRING_BACK_FAST`: spring_back_fast as
  /// a scheduled V0 replacement instead of a zero-stamp one.
  bool scheduled_spring_back_fast{false};
  /// `MANYFORGE_EXPERIMENTAL_RELEASE_TIMER_LATENCY_MS`: send to callback
  /// completion plus `controller_state` transport (the receipt mapping's bias).
  double expected_latency_s{0.0015};
  /// Diagnostic only: the effective allowance remains the Stop config's value.
  bool start_allowance_override{false};

  [[nodiscard]] bool any() const {
    return release_timer || midtick_stamp || v0_payload || scheduled_spring_back_fast ||
           start_allowance_override;
  }
  friend bool operator==(const ExecutionReplacementExperimentFlagsDTO&,
                         const ExecutionReplacementExperimentFlagsDTO&) = default;
};

/// Static facts of the bound lane; fixed per binding epoch.
struct ExecutionLaneFactsDTO final {
  /// The binding's controller joint order. Sample vectors follow it;
  /// submitted trajectories use the same joint set (the adapter maps by name).
  std::vector<std::string> joint_names;
  bool supports_scale{false};
  bool has_measured_velocity{false};
  ExecutionCancelSemantics cancel_semantics{ExecutionCancelSemantics::kDecelerate};
  /// Nominal action-feedback and `controller_state` rates.
  double feedback_rate_hz{0.0};
  double state_rate_hz{0.0};
  /// Nominal controller-manager update rate; Core publishes the speed ramp at
  /// this rate (Q11).
  double controller_update_rate_hz{0.0};
  ExecutionGoalTolerancePolicy goal_tolerance_policy{ExecutionGoalTolerancePolicy::kErased};
  /// Qualified binding supports one atomic replacement trajectory/start stamp
  /// command. Unqualified lanes retain their existing immediate replacement.
  bool supports_scheduled_replacement{false};
  /// EXPERIMENTAL, default off; acts only with `supports_scheduled_replacement`.
  ExecutionReplacementExperimentFlagsDTO experiment{};

  friend bool operator==(const ExecutionLaneFactsDTO&,
                         const ExecutionLaneFactsDTO&) = default;
};

// ---------------------------------------------------------------------------
// Port
// ---------------------------------------------------------------------------

/// Execution port for one robot participant.
///
/// Non-blocking contract: no call waits on the controller. Commands send and
/// return; reads return the latest received facts. Calls may come from any
/// host thread, including transport callbacks.
class IExecutionPort {
 public:
  virtual ~IExecutionPort() = default;

  /// Start a goal. Waypoint times are path time; positions, velocities and
  /// accelerations are required (Core's sampler fills them). The joint set is
  /// that of `ReadLaneFacts().joint_names`; the adapter maps by name.
  virtual ExecutionPortCommandResultDTO Submit(const ExecutionGoalKey& key,
                                               const TrajectoryDTO& trajectory) = 0;

  /// Replace the active goal with a trajectory that continues it (soft Stop
  /// ramp, spring-back). Transport-neutral: the trajectory may start with
  /// points at or before the mechanism's current time, so the mechanism keeps
  /// following the original points through the lead-in. ROS realization (Q20,
  /// ManyMove's past-point join): Core keeps the original trajectory's past
  /// points with all times shifted by the elapsed time (negative times on the
  /// wire, never clamped), followed by the Stop ramp; the adapter serializes
  /// them faithfully. `kStaleKey` when `active_key` is not the current goal.
  virtual ExecutionPortCommandResultDTO Replace(const ExecutionGoalKey& active_key,
                                                const ExecutionGoalKey& next_key,
                                                const TrajectoryDTO& trajectory) = 0;

  /// Same identity/prefix contract as Replace, with its time-zero anchored to
  /// the named binding's controller clock. Refusal is explicit; silently
  /// falling back to an immediate send would change the chosen curve origin.
  virtual ExecutionPortCommandResultDTO ReplaceScheduled(
      const ExecutionGoalKey& /*active_key*/, const ExecutionGoalKey& /*next_key*/,
      const TrajectoryDTO& /*trajectory*/, const ExecutionReplacementStartDTO& /*start*/) {
    return {ExecutionPortDispatch::kNotDispatched, ExecutionPortRefusal::kUnsupported,
            std::nullopt};
  }

  /// The lane's hard stop (Q2). The acknowledgement never implies rest: the
  /// finish is decided by Core (Q5, Q35; see the cancel
  /// association on `ExecutionControllerSampleDTO`). On a goal still pending
  /// (no mechanism handle yet) the mechanism latches the cancel and sends it
  /// on acceptance, returning `kEffectUncertain` meanwhile. A refused or
  /// unsent cancel is never containment evidence.
  virtual ExecutionPortCommandResultDTO Cancel(const ExecutionGoalKey& key) = 0;

  /// Publish one speed factor in [0, 1] (Q11). Values apply in send order on
  /// one binding epoch; a new epoch starts at the configured initial factor.
  /// The applied factor is read back from `ReadSample()`.
  virtual ExecutionPortCommandResultDTO SetScale(double value) = 0;

  /// Goal-scoped facts; nullopt for a key never sent through this port or
  /// whose record left the ring (`kExecutionGoalRecordCapacity`).
  virtual std::optional<ExecutionGoalObservationDTO> ReadGoal(
      const ExecutionGoalKey& key) const = 0;

  /// Latest controller-wide sample; nullopt before the first one on the
  /// current binding.
  virtual std::optional<ExecutionControllerSampleDTO> ReadSample() const = 0;

  virtual ExecutionReadinessDTO ReadReadiness() const = 0;

  /// Timing since the previous call; starts a new window.
  virtual ExecutionTimingDTO ReadTiming() = 0;

  virtual ExecutionLaneFactsDTO ReadLaneFacts() const = 0;
};

}  // namespace manyforge::core
