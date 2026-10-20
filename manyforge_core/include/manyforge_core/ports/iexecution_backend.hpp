#pragma once

#include <manyforge_core/execution_progress_publication.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/prepared_execution.hpp>
#include <manyforge_core/types/runtime_scale.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace manyforge::core {

/// A critical internal fault of a dispatched execution (item 17; Q52: a
/// controller cycle later than the lane margin). A backend reports it by
/// throwing it from the execution handle; the Kernel's item-17 path issues
/// the lane's hard stop (`stop()`) and ends the motion InterventionRequired
/// with `error`, exactly as for a worker exception after dispatch.
class ExecutionCriticalFault final : public std::runtime_error {
 public:
  explicit ExecutionCriticalFault(ErrorDTO error)
      : std::runtime_error(error.message), error_(std::move(error)) {}
  [[nodiscard]] const ErrorDTO& error() const noexcept { return error_; }

 private:
  ErrorDTO error_;
};

/// Execution backend interface (spec 220 §4)
///
/// Responsibilities:
/// - Execute a time-parameterized joint trajectory
/// - Provide phase feedback for kernel events
/// - Provide stop/reset primitives with bounded kernel semantics
///
/// Threading:
/// - Stop admission MUST be non-blocking from the kernel caller perspective
/// - Reset MUST be prompt during planning/validation cancellation, but may wait
///   for a bounded stop-before-reset sequence while the robot is moving
/// - Stop/Reset MUST be safe to call from a different thread than ExecuteTrajectory
/// - The kernel MUST NOT issue concurrent ExecuteTrajectory calls to the same backend instance
class IExecutionBackend : public IControlDomainExecutionPort {
 public:
  virtual ~IExecutionBackend() = default;

  /// Side-effect-free exact participant preparation. Implementations accept
  /// only the motion_execution payload. execution_id is the owning Kernel's
  /// existing monotonic execution ID.
  /// The backend and that ID owner share a lifetime. Retain current membership
  /// only; preparation never sends a transport command.
  virtual PrepareParticipantResultDTO Prepare(const PreparedOperationParticipantDTO& participant,
                                              std::uint64_t execution_id) = 0;

  /// Owner-authorized retirement after worker, dispatch, Stop and recovery
  /// quiescence. Never stops motion or resets observed state. Old requests must
  /// not reopen retired membership, and stale retirement cannot clear a newer
  /// execution. Failure retains membership and prevents owner release.
  virtual Result<void> RetirePreparedExecution(std::uint64_t execution_id) = 0;

  /// Optional source-fenced takeover on this same execution owner. Unsupported
  /// backends retain ordinary preparation/dispatch behavior unchanged.
  virtual Result<PreparedMotionReplacementSourceDTO> CaptureReplacementSource(
      std::uint64_t /*execution_id*/, std::uint64_t /*execution_generation*/,
      const ExecutionGoalKey& /*goal*/, const ContentDigest& /*digest*/) const {
    return Result<PreparedMotionReplacementSourceDTO>::Err(ErrorCode::kUnsupportedCapability);
  }

  /// Owner-fenced host scaling hold, charged to the existing preparation
  /// deadline. Capture remains read-only; Stop freeze remains independently owned.
  virtual Result<void> BeginReplacementPreparationScaleHold(
      const PreparedMotionReplacementSourceDTO&, std::uint64_t,
      std::int64_t, std::stop_token) {
    return Result<void>::Err(ErrorCode::kUnsupportedCapability);
  }
  virtual Result<void> EndReplacementPreparationScaleHold(std::uint64_t) {
    return Result<void>::Ok();
  }

  virtual PrepareParticipantResultDTO PrepareReplacement(
      const PreparedOperationParticipantDTO& participant, std::uint64_t /*candidate_id*/,
      const PreparedMotionReplacementSourceDTO& /*source*/) {
    return MakePrepareParticipantResult(
               participant, PrepareParticipantResultStateDTO::kRejected,
               ErrorDTO{kErrorUnsupportedCapability, "Motion replacement is unsupported", {}})
        .value();
  }

  virtual PreparedMotionReplacementDispatchDTO DispatchReplacement(
      const PreparedMotionReplacementSourceDTO& /*source*/, std::uint64_t /*candidate_id*/,
      const PreparedOperationParticipantDTO& /*participant*/,
      const ExecutionGoalKey& /*successor_goal*/, std::int64_t /*preparation_deadline_ns*/) {
    return {{ExecutionPortDispatch::kNotDispatched, ExecutionPortRefusal::kUnsupported,
             ErrorDTO{kErrorUnsupportedCapability, "Motion replacement is unsupported", {}}},
            std::nullopt};
  }

  virtual Result<PreparedMotionReplacementObservationDTO> ObserveReplacement(
      const PreparedMotionReplacementSourceDTO& /*source*/, std::uint64_t /*candidate_id*/) const {
    return Result<PreparedMotionReplacementObservationDTO>::Err(ErrorCode::kUnsupportedCapability);
  }

  virtual Result<void> AdoptReplacement(const PreparedMotionReplacementSourceDTO& /*source*/,
                                        std::uint64_t /*candidate_id*/,
                                        PreparedMotionReplacementCommitDTO /*commit*/) {
    return Result<void>::Err(ErrorCode::kUnsupportedCapability);
  }

  /// Dispatch one already-prepared participant under the Kernel-minted goal.
  /// Exact repetition returns the same goal-bound handle and never sends a
  /// second transport command.
  virtual ExecutionHandle Dispatch(const PreparedOperationParticipantDTO& participant,
                                   const ControllerGoalId& controller_goal_id,
                                   ControllerGoalGeneration controller_goal_generation) = 0;

  /// Execute a trajectory (spec 220 §4)
  ///
  /// @param trajectory The trajectory to execute
  /// @return ExecutionHandle for tracking the execution
  virtual ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) = 0;

  /// Synchronize the backend-owned planning scene from the authoritative
  /// observed execution state before planning.
  ///
  /// Native/offline execution backends may treat this as a no-op.
  /// Real/observed controller-backed adapters use it as the hard gate between
  /// measured robot state and planning: if a fresh complete observed state
  /// cannot be obtained, planning must not proceed from stale authored or
  /// commanded state. Command-authoritative simulation profiles instead
  /// synchronize their backend-authored state under their deployment
  /// contract; controller feedback remains conformance evidence there.
  virtual Result<void> sync_observed_state_before_planning() = 0;

  /// Stop the current execution (spec 220 §4)
  ///
  /// If no execution is in flight, returns accepted=false with invalid_request.
  ///
  /// @return StopResultDTO indicating whether stop was accepted
  virtual StopResultDTO stop() = 0;

  /// Reset the execution backend (spec 220 §4)
  ///
  /// @return ResetResultDTO indicating whether reset was accepted
  virtual ResetResultDTO reset() = 0;

  /// Return the latest observer-safe backend snapshot.
  ///
  /// This is intentionally part of the execution backend contract rather
  /// than an implementation detail: controller-backed adapters must expose
  /// observed/predicted execution state without forcing the kernel or UI to
  /// know which ROS topics/actions produced it.
  virtual ExecutionSnapshot get_execution_snapshot() const = 0;

  /// Dedicated observed-robot-state publication (spec/210 section 3.13).
  ///
  /// This is deliberately separate from get_execution_snapshot(): lifecycle
  /// state is not robot-state authority. The returned object is backend-owned,
  /// immutable to the kernel, and must outlive every kernel call. A null
  /// result means observed-state-dependent collision/recovery checks fail
  /// closed; production backends are required to override it.
  virtual const ObservedRobotStatePublication* observed_robot_state_publication() const noexcept {
    return nullptr;
  }

  /// Dedicated exact trajectory-time cursor publication (spec/210 §3.14).
  /// This is distinct from lifecycle progress and observed robot state. A
  /// null result means generation-fenced runtime lookahead is unavailable.
  virtual const ExecutionProgressPublication* execution_progress_publication() const noexcept {
    return nullptr;
  }

  /// Original accepted-trajectory cursor retained at completed Stop.
  /// This bounded local read is scoped to the exact dispatch generation. It
  /// does not renew lookahead liveness or prove settlement/admission. Missing
  /// or uncertain correspondence returns nullopt, never replacement progress.
  virtual std::optional<double> stopped_trajectory_time_s(
      std::uint64_t /*execution_generation*/) const {
    return std::nullopt;
  }

  /// Retire exactly one backend progress generation after the kernel has
  /// disarmed every runtime-lookahead consumer for that execution. Stop,
  /// reset, failure, and authority-loss paths still invalidate eagerly;
  /// this handoff exists so natural completion cannot masquerade as cursor
  /// loss in the backend-result -> kernel-terminal interval.
  virtual void retire_execution_progress_generation(
      std::uint64_t execution_generation) noexcept = 0;

  /// Atomically arbitrate runtime-lookahead stop authority against natural
  /// completion for the exact backend dispatch generation.
  virtual ExecutionProgressHazardClaimResult claim_execution_progress_hazard(
      std::uint64_t execution_generation) noexcept = 0;

  /// Update the runtime velocity scale used by active execution.
  ///
  /// ADR-0018: this is a live control surface, not a trajectory planning
  /// parameter. Implementations SHOULD use an atomic / lock-free store so
  /// discrete trajectory workers can read it cheaply on every tick. A scale
  /// of 0.0 means "hold trajectory time" rather than Stop. Inputs MUST be
  /// finite and clamped to [0, 1]; the kernel validates before forwarding and
  /// implementations should still defend in depth.
  virtual void set_runtime_velocity_scale(double /*scale*/) {
    // Default: no-op. Backends that support runtime velocity scaling override.
  }

  /// Observer-safe acknowledgement/application status for the private
  /// mechanism. `supported=false` is the fail-closed default; retained
  /// binding qualification is a separate mandatory admission check.
  virtual RuntimeScaleMechanismStatusDTO runtime_velocity_scale_status() const {
    return RuntimeScaleMechanismStatusDTO{};
  }

  // ---------------------------------------------------------------
  // Controlled-stop capability surface (spec/240).
  //
  // The kernel reads this once at arm time to enforce pre-dispatch
  // capability gating on `complete_move_on_signal` scopes. Default
  // returns an empty capability set so backends that have not
  // implemented the controlled-stop contract fail closed: any
  // execute request carrying a move-scoped trip policy is rejected
  // with `motion_interrupt_arm_failed` and reason
  // `stop_mode_not_supported_by_backend`. Backends override once
  // they declare at least one production-qualified stop mode in
  // their capability profile.
  // ---------------------------------------------------------------
  virtual MotionInterruptStopCapabilityDTO get_motion_interrupt_stop_capability() const {
    return MotionInterruptStopCapabilityDTO{};
  }

  // ---------------------------------------------------------------
  // Controlled-stop result surface (spec/260 §5.6).
  //
  // Called by the kernel at trip time when a `complete_move_on_signal`
  // latch fires. The backend MUST execute the requested stop mode (or the
  // sole explicitly authorized and qualified recoverable substitution),
  // wait for the controller within the assigned Kernel Stop envelope, and return a
  // `ControlledStopResultDTO` describing what actually happened. The
  // kernel validates exact requested/executed provenance on top of this
  // result and downgrades the outcome to `failed` on any deviation.
  //
  // The default implementation returns `accepted=false` with a
  // `motion_interrupt_backend_stop_failed` error so backends that
  // have not yet implemented the controlled-stop contract fail
  // closed. The capability gate also rejects these
  // backends at arm time when the supported_stop_modes set is empty;
  // this default exists for backends that mistakenly declare a
  // non-empty capability set without overriding this method.
  // ---------------------------------------------------------------
  virtual ControlledStopResultDTO execute_controlled_stop(
      const MotionInterruptStopProfileDTO& /*profile*/) {
    ControlledStopResultDTO result;
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                            "execute_controlled_stop is not implemented by this backend",
                            {{"reason", "controlled_stop_port_not_implemented"}}};
    return result;
  }

  /// Cancellable controlled-stop variant used by operator interventions.
  ///
  /// The default keeps source compatibility for existing backends. Backends
  /// with bounded waits or replay loops should override this overload and
  /// poll the stop token so a later stop/reset can preempt a soft stop.
  virtual ControlledStopResultDTO execute_controlled_stop(
      const MotionInterruptStopProfileDTO& profile, std::stop_token /*stop_token*/) {
    return execute_controlled_stop(profile);
  }
};

}  // namespace manyforge::core
