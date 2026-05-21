#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_core/types/streaming_session.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Execution backend interface (spec 220 §4)
/// 
/// Responsibilities:
/// - Execute a time-parameterized joint trajectory
/// - Provide phase feedback for kernel events
/// - Provide non-blocking stop/pause/resume/reset primitives
///
/// Threading:
/// - Stop/Pause/Resume/Reset MUST be non-blocking from the kernel caller perspective
/// - Stop/Pause/Resume/Reset MUST be safe to call from a different thread than ExecuteTrajectory
/// - The kernel MUST NOT issue concurrent ExecuteTrajectory calls to the same backend instance
class IExecutionBackend {
 public:
    virtual ~IExecutionBackend() = default;

    /// Execute a trajectory (spec 220 §4)
    /// 
    /// @param trajectory The trajectory to execute
    /// @return ExecutionHandle for tracking the execution
    virtual ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) = 0;

    /// Synchronize the backend-owned planning scene from the authoritative
    /// observed execution state before planning.
    ///
    /// Native/offline execution backends may treat this as a no-op. Controller
    /// backed adapters use it as the hard gate between live robot/simulator
    /// state and planning: if a fresh complete observed state cannot be
    /// obtained, planning must not proceed from stale authored state.
    virtual Result<void> sync_observed_state_before_planning() = 0;

    /// Stop the current execution (spec 220 §4)
    /// 
    /// If no execution is in flight, returns accepted=false with invalid_request.
    /// 
    /// @return StopResultDTO indicating whether stop was accepted
    virtual StopResultDTO stop() = 0;

    /// Pause the current execution (spec 220 §4)
    /// 
    /// If no execution is in flight, returns accepted=false with invalid_request.
    /// 
    /// @return PauseResultDTO indicating whether pause was accepted
    virtual PauseResultDTO pause() = 0;

    /// Resume execution with a new trajectory (spec 220 §4)
    /// 
    /// Resume takes a trajectory because the kernel may replan from the paused state;
    /// the backend executes it as a fresh trajectory dispatch.
    /// 
    /// @param trajectory The trajectory to resume with
    /// @return ExecutionHandle for tracking the resumed execution
    virtual ExecutionHandle resume(const TrajectoryDTO& trajectory) = 0;

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

    // ---------------------------------------------------------------
    // Streaming-session mode (ADR-0016).
    //
    // These operations are additive on top of the discrete-trajectory
    // contract above. They are mutually exclusive with execute_trajectory:
    // the backend MUST NOT host an active trajectory execution and an
    // active streaming session simultaneously.
    //
    // Default implementations return invalid_request so backends that have
    // not opted into the streaming-session mode keep building. Backends
    // that DO support streaming override all three.
    // ---------------------------------------------------------------

    /// Open a streaming session (ADR-0016 §"Surface").
    ///
    /// On accept, the backend prepares its target register and any
    /// controller-side state required to dispatch rolling-horizon goals,
    /// and returns a backend-assigned session_id. The kernel transitions
    /// kIdle -> kStreaming on accept.
    virtual StreamingSessionBeginResultDTO begin_streaming_session(
        const StreamingSessionConfigDTO& /*config*/) {
      StreamingSessionBeginResultDTO result;
      result.accepted = false;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "streaming-session mode not supported by this execution backend",
          {}};
      return result;
    }

    /// Update the active streaming session's current target.
    ///
    /// Latest-writes-win; targets that arrive faster than the backend can
    /// dispatch are coalesced. MUST NOT block on motion completion. Returns
    /// a backend-assigned sequence number that downstream
    /// streaming_target_dispatched events correlate with.
    virtual StreamingPushResultDTO push_streaming_target(
        uint64_t /*session_id*/,
        const StreamingTargetDTO& /*target*/) {
      StreamingPushResultDTO result;
      result.accepted = false;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "streaming-session mode not supported by this execution backend",
          {}};
      return result;
    }

    /// End the streaming session using the supplied soft-stop strategy.
    ///
    /// The backend runs the soft-stop trajectory (or velocity-scaling ramp)
    /// per the strategy. The session reaches terminal state once the
    /// soft-stop completes; the kernel transitions kStreaming -> kStopping
    /// -> kStopped via the standard state-machine path.
    virtual StreamingSessionEndResultDTO end_streaming_session(
        uint64_t /*session_id*/,
        SoftStopStrategy /*strategy*/) {
      StreamingSessionEndResultDTO result;
      result.accepted = false;
      result.error = ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "streaming-session mode not supported by this execution backend",
          {}};
      return result;
    }

    /// Update the runtime velocity scale used by active execution.
    ///
    /// ADR-0018: this is a live control surface, not a trajectory planning
    /// parameter. Implementations SHOULD use an atomic / lock-free store so
    /// discrete trajectory workers and streaming workers can read it cheaply on
    /// every tick. A scale of 0.0 means "hold trajectory time" rather than Stop.
    /// Inputs MUST be finite and clamped to [0, 1]; the kernel validates before
    /// forwarding and implementations should still defend in depth.
    virtual void set_runtime_velocity_scale(double /*scale*/) {
      // Default: no-op. Backends that support runtime velocity scaling override.
    }

    /// Compatibility alias for older callers.
    virtual void set_streaming_velocity_scale(double scale) {
      set_runtime_velocity_scale(scale);
    }
};

}  // namespace manyforge::core
